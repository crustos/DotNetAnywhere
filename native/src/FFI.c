// See FFI.h

#include "Compat.h"
#include "Sys.h"

#include "FFI.h"
#include "MetaDataTables.h"
#include "Type.h"
#include "EvalStack.h"
#include "System.String.h"
#include "System.Array.h"

// (ffiTable and ffiCount are defined in FFIDefault.c, or in the FFI.gen.c of a build with a manifest)

U32 FFI_Count(void) {
	return ffiCount;
}

// A library is named "mylib", "libmylib", "libmylib.so", "mylib.dll"...: compare what is left without the prefix and suffix
static void BaseName(const char *in, char *out, size_t cap) {
	size_t len;
	const char *slash = strrchr(in, '/');
	if (slash != NULL) { in = slash + 1; }
	if (strncmp(in, "lib", 3) == 0 && in[3] != 0) { in += 3; }
	strncpy(out, in, cap - 1);
	out[cap - 1] = 0;
	len = strlen(out);
	if (len > 3 && strcmp(out + len - 3, ".so") == 0) { out[len - 3] = 0; }
	else if (len > 4 && strcmp(out + len - 4, ".dll") == 0) { out[len - 4] = 0; }
	else if (len > 6 && strcmp(out + len - 6, ".dylib") == 0) { out[len - 6] = 0; }
}

const tFFIEntry* FFI_Find(const char *library, const char *entry) {
	char want[128], have[128];
	U32 i;
	if (ffiCount == 0 || library == NULL || entry == NULL) {
		return NULL;
	}
	BaseName(library, want, sizeof(want));
	for (i = 0; i < ffiCount; i++) {
		BaseName(ffiTable[i].library, have, sizeof(have));
		if (strcmp(have, want) == 0 && strcmp(ffiTable[i].entry, entry) == 0) {
			return &ffiTable[i];
		}
	}
	return NULL;
}

// The letter for a C# type in a signature, or 0 if it cannot be passed to a C function directly
static char KindOf(tMD_TypeDef *pType) {
	if (pType == NULL) { return 'v'; }
	MetaData_Fill_TypeDef(pType, NULL, NULL);
	if (pType == types[TYPE_SYSTEM_INTPTR] || pType == types[TYPE_SYSTEM_UINTPTR]) { return 'p'; }     // (a ref or out is typed as an IntPtr here)
	if (pType == types[TYPE_SYSTEM_STRING]) { return 's'; }
	if (TYPE_ISARRAY(pType)) { return 'b'; }
	switch (pType->stackType) {
	case EVALSTACK_INT32: return 'i';
	case EVALSTACK_INT64: return 'l';
	case EVALSTACK_F32: return 'f';
	case EVALSTACK_F64: return 'd';
	case EVALSTACK_INTNATIVE: return 'p';
	case EVALSTACK_PTR: return 'p';
	default: return 0;
	}
}

const char* FFI_CheckSignature(const tFFIEntry *pEntry, tMD_MethodDef *pMethod) {
	static char msg[200];
	U32 i;
	char k;
	if (pMethod->numberOfParameters != strlen(pEntry->args)) {
		sprintf(msg, "%s declares %d parameter(s), the FFI manifest says %d", pEntry->entry, (int)pMethod->numberOfParameters, (int)strlen(pEntry->args));
		return msg;
	}
	for (i = 0; i < pMethod->numberOfParameters; i++) {
		k = KindOf(pMethod->pParams[i].pTypeDef);
		if (k == 0 || k != pEntry->args[i]) {
			sprintf(msg, "%s: parameter %d is '%s' in C# (kind %c), the FFI manifest says kind %c", pEntry->entry, (int)i + 1,
				pMethod->pParams[i].pTypeDef->name, k ? k : '?', pEntry->args[i]);
			return msg;
		}
	}
	k = KindOf(pMethod->pReturnType);
	if (k == 0 || k != pEntry->ret) {
		sprintf(msg, "%s: the result is kind %c in C#, the FFI manifest says kind %c", pEntry->entry, k ? k : '?', pEntry->ret);
		return msg;
	}
	return NULL;
}

const char* FFI_StringToUtf8(HEAP_PTR pString, char *buf, size_t cap, int *pHeap) {
	U32 len, i, n = 0;
	STRING2 str;
	char *out;
	*pHeap = 0;
	if (pString == NULL) {
		return NULL;
	}
	str = SystemString_GetString(pString, &len);
	// how many bytes: 1 to 3 for a UTF-16 unit, 4 for a surrogate pair
	for (i = 0; i < len; i++) {
		U32 c = str[i];
		if (c >= 0xd800 && c < 0xdc00 && i + 1 < len && str[i + 1] >= 0xdc00 && str[i + 1] < 0xe000) { n += 4; i++; }
		else { n += (c < 0x80) ? 1 : (c < 0x800) ? 2 : 3; }
	}
	if (n + 1 <= cap) {
		out = buf;
	} else {
		out = (char*)malloc(n + 1);
		*pHeap = 1;
	}
	n = 0;
	for (i = 0; i < len; i++) {
		U32 c = str[i];
		if (c >= 0xd800 && c < 0xdc00 && i + 1 < len && str[i + 1] >= 0xdc00 && str[i + 1] < 0xe000) {
			c = 0x10000 + ((c - 0xd800) << 10) + (str[i + 1] - 0xdc00);
			i++;
		} else if (c >= 0xd800 && c < 0xe000) {
			c = 0xfffd;                          // an unpaired surrogate
		}
		if (c < 0x80) { out[n++] = (char)c; }
		else if (c < 0x800) { out[n++] = (char)(0xc0 | (c >> 6)); out[n++] = (char)(0x80 | (c & 0x3f)); }
		else if (c < 0x10000) { out[n++] = (char)(0xe0 | (c >> 12)); out[n++] = (char)(0x80 | ((c >> 6) & 0x3f)); out[n++] = (char)(0x80 | (c & 0x3f)); }
		else { out[n++] = (char)(0xf0 | (c >> 18)); out[n++] = (char)(0x80 | ((c >> 12) & 0x3f)); out[n++] = (char)(0x80 | ((c >> 6) & 0x3f)); out[n++] = (char)(0x80 | (c & 0x3f)); }
	}
	out[n] = 0;
	return out;
}

HEAP_PTR FFI_StringFromUtf8(const char *pUtf8) {
	const unsigned char *p = (const unsigned char*)pUtf8;
	size_t len = strlen(pUtf8), n = 0, i = 0;
	U16 *u = (U16*)malloc((len + 1) * sizeof(U16));       // (never more units than bytes)
	HEAP_PTR ret;
	while (i < len) {
		U32 c = p[i], extra = 0, min = 0;
		if (c < 0x80) { extra = 0; }
		else if (c >= 0xc2 && c < 0xe0) { c &= 0x1f; extra = 1; min = 0x80; }
		else if (c >= 0xe0 && c < 0xf0) { c &= 0x0f; extra = 2; min = 0x800; }
		else if (c >= 0xf0 && c < 0xf5) { c &= 0x07; extra = 3; min = 0x10000; }
		else { u[n++] = 0xfffd; i++; continue; }
		{
			size_t k;
			int bad = 0;
			for (k = 1; k <= extra && !bad; k++) {
				if (i + k >= len || (p[i + k] & 0xc0) != 0x80) { bad = 1; } else { c = (c << 6) | (p[i + k] & 0x3f); }
			}
			if (bad || c < min || c > 0x10ffff || (c >= 0xd800 && c < 0xe000)) { u[n++] = 0xfffd; i++; continue; }
			i += extra + 1;
		}
		if (c >= 0x10000) { c -= 0x10000; u[n++] = (U16)(0xd800 + (c >> 10)); u[n++] = (U16)(0xdc00 + (c & 0x3ff)); }
		else { u[n++] = (U16)c; }
	}
	u[n] = 0;
	ret = SystemString_FromCharPtrUTF16(u);
	free(u);
	return ret;
}
