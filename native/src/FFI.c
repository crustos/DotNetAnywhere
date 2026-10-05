// See FFI.h

#include "Compat.h"
#include "Sys.h"

#include "FFI.h"
#include "MetaDataTables.h"
#include "Type.h"
#include "EvalStack.h"
#include "System.String.h"
#include "System.Array.h"
#include "Thread.h"
#include "Delegate.h"
#include "Host.h"

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
char FFI_KindOf(tMD_TypeDef *pType) {
	if (pType == NULL) { return 'v'; }
	MetaData_Fill_TypeDef(pType, NULL, NULL);
	if (pType == types[TYPE_SYSTEM_INTPTR] || pType == types[TYPE_SYSTEM_UINTPTR]) { return 'p'; }     // (a ref or out is typed as an IntPtr here)
	if (pType == types[TYPE_SYSTEM_STRING]) { return 's'; }
	if (TYPE_ISARRAY(pType)) { return 'b'; }
	if (pType->isValueType && pType->stackType == EVALSTACK_VALUETYPE) { return 'S'; }
	if (pType->pParent != NULL && pType->pParent == types[TYPE_SYSTEM_MULTICASTDELEGATE]) { return 'c'; }      // a delegate: C gets a function pointer that calls it       // a struct of the program's own (not a number: those are INT32 and so on)
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

const char* FFI_CheckStruct(const tFFIStruct *pC, tMD_TypeDef *pType) {
	static char msg[800];
	U32 i, n = 0;
	MetaData_Fill_TypeDef(pType, NULL, NULL);
	if (!pType->isValueType || pType->stackType != EVALSTACK_VALUETYPE) {
		snprintf(msg, sizeof(msg), "%s is not a struct, the manifest says the C struct %s", pType->name, pC->name);
		return msg;
	}
	if (pType->instanceMemSize != pC->size) {
		U32 j, narrow = 0;
		for (j = 0; j < pC->numFields; j++) { if (pC->fields[j].kind == 'i' && pC->fields[j].size < 4) { narrow = 1; } }
		snprintf(msg, sizeof(msg), "%s is %d bytes in C# and the C struct %s is %d%s", pType->name, (int)pType->instanceMemSize, pC->name, (int)pC->size,
			narrow ? " (a field narrower than 4 bytes (byte, short, bool) takes a 4-byte slot in a struct here, so it cannot match C: use int)"
			       : " (C pads the end of a struct to its largest alignment, and a struct here is padded only to 8 when it has an 8-byte field)");
		return msg;
	}
	for (i = 0; i < pType->numFields; i++) {
		tMD_FieldDef *pField = pType->ppFields[i];
		const tFFIField *pCField;
		char k;
		if (pField == NULL || FIELD_ISSTATIC(pField) || FIELD_ISLITERAL(pField)) { continue; }      // (only what takes room in the struct)
		if (n >= pC->numFields) {
			snprintf(msg, sizeof(msg), "%s has more fields than the C struct %s (%d)", pType->name, pC->name, (int)pC->numFields);
			return msg;
		}
		pCField = &pC->fields[n];
		k = FFI_KindOf(pField->pType);
		if (pField->memOffset != pCField->offset || pField->memSize != pCField->size || k != pCField->kind) {
			snprintf(msg, sizeof(msg), "%s.%s (field %d) is a %c of %d bytes at offset %d in C#; the C struct %s has %s, a %c of %d bytes at offset %d%s", pType->name, pField->name,
				(int)n + 1, k ? k : '?', (int)pField->memSize, (int)pField->memOffset, pC->name, pCField->name, pCField->kind, (int)pCField->size, (int)pCField->offset,
				(pCField->kind == 'i' && pCField->size < 4 && pField->memSize == 4) ? " (a field narrower than 4 bytes (byte, short, bool) takes a 4-byte slot in a struct here, so it cannot match C: use int)" : "");
			return msg;
		}
		n++;
	}
	if (n != pC->numFields) {
		snprintf(msg, sizeof(msg), "%s has %d field(s), the C struct %s has %d", pType->name, (int)n, pC->name, (int)pC->numFields);
		return msg;
	}
	return NULL;
}

const char* FFI_CheckSignature(const tFFIEntry *pEntry, tMD_MethodDef *pMethod) {
	static char msg[900];
	U32 i;
	char k;
	if (pMethod->numberOfParameters != strlen(pEntry->args)) {
		snprintf(msg, sizeof(msg), "%s declares %d parameter(s), the FFI manifest says %d", pEntry->entry, (int)pMethod->numberOfParameters, (int)strlen(pEntry->args));
		return msg;
	}
	for (i = 0; i < pMethod->numberOfParameters; i++) {
		k = FFI_KindOf(pMethod->pParams[i].pTypeDef);
		if (k == 0 || k != pEntry->args[i]) {
			snprintf(msg, sizeof(msg), "%s: parameter %d is '%s' in C# (kind %c), the FFI manifest says kind %c", pEntry->entry, (int)i + 1,
				pMethod->pParams[i].pTypeDef->name, k ? k : '?', pEntry->args[i]);
			return msg;
		}
		if (k == 'c' && pEntry->argCallbacks != NULL && pEntry->argCallbacks[i] != NULL) {
			const char *pWhy = FFI_CheckCallback(pEntry->argCallbacks[i], pMethod->pParams[i].pTypeDef);
			if (pWhy != NULL) {
				snprintf(msg, sizeof(msg), "%s: parameter %d: %s", pEntry->entry, (int)i + 1, pWhy);
				return msg;
			}
		}
		if (pEntry->argStructs != NULL && pEntry->argStructs[i] != NULL) {
			// a struct by value, or an array of structs (a ref to one is typed as a pointer here, so its layout cannot be checked)
			tMD_TypeDef *pSt = (k == 'b') ? pMethod->pParams[i].pTypeDef->pArrayElementType : (k == 'S') ? pMethod->pParams[i].pTypeDef : NULL;
			if (pSt != NULL) {
				const char *pWhy = FFI_CheckStruct(pEntry->argStructs[i], pSt);
				if (pWhy != NULL) {
					snprintf(msg, sizeof(msg), "%s: parameter %d: %s", pEntry->entry, (int)i + 1, pWhy);
					return msg;
				}
			}
		}
	}
	k = FFI_KindOf(pMethod->pReturnType);
	if (k == 'S' && pEntry->ret == 'S' && pEntry->retStruct != NULL) {
		const char *pWhy = FFI_CheckStruct(pEntry->retStruct, pMethod->pReturnType);
		if (pWhy != NULL) {
			snprintf(msg, sizeof(msg), "%s: the result: %s", pEntry->entry, pWhy);
			return msg;
		}
	}
	if (k == 0 || k != pEntry->ret) {
		snprintf(msg, sizeof(msg), "%s: the result is kind %c in C#, the FFI manifest says kind %c", pEntry->entry, k ? k : '?', pEntry->ret);
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


// ---- callbacks ------------------------------------------------------------------------------------------------------------------------------

int FFI_CallbackAcquire(const tFFICallback *cb, HEAP_PTR delegate) {
	U32 i;
	if (delegate == NULL) {
		return -1;
	}
	for (i = 0; i < cb->slots; i++) {
		if (cb->delegates[i] == NULL) {
			cb->delegates[i] = delegate;
			cb->handles[i] = Host_Pin(delegate);        // a root: the delegate and its target stay alive while C can call it
			return (int)i;
		}
	}
	Crash("more than %d callbacks of type %s are in use at once", (int)cb->slots, cb->name);
	return -1;
}

void FFI_CallbackRelease(const tFFICallback *cb, int slot) {
	if (slot < 0) {
		return;
	}
	DNA_Release(cb->handles[slot]);
	cb->handles[slot] = 0;
	cb->delegates[slot] = NULL;
}

static void PutArg(U8 *at, char kind, const tFFIValue *v) {
	switch (kind) {
	case 'i': *(int32_t*)at = v->i; break;
	case 'l': *(int64_t*)at = v->l; break;
	case 'f': *(float*)at = v->f; break;
	case 'd': *(double*)at = v->d; break;
	default:  *(intptr_t*)at = (intptr_t)v->p; break;
	}
}

tFFIValue FFI_CallbackInvoke(const tFFICallback *cb, U32 slot, const tFFIValue *args) {
	tFFIValue result;
	HEAP_PTR d = cb->delegates[slot];
	memset(&result, 0, sizeof(result));
	while (d != NULL) {
		// (a delegate that was combined with others calls each in turn, and the result is the last one's, as in C#)
		HEAP_PTR target = NULL;
		void *pNext = NULL;
		tMD_MethodDef *m = Delegate_GetMethodAndStore(d, &target, &pNext);
		U8 small[128];
		U8 *pbuf = small;
		U32 k, first = 0;
		tThread *pThread;
		if (!m->isFilled) {
			MetaData_Fill_TypeDef(m->pParentType, NULL, NULL);
			MetaData_Fill_MethodDef(m->pParentType, m, NULL, NULL);
		}
		if (m->parameterStackSize > sizeof(small)) {
			pbuf = (U8*)malloc(m->parameterStackSize);
		}
		memset(pbuf, 0, m->parameterStackSize);
		if (!METHOD_ISSTATIC(m)) {
			*(HEAP_PTR*)(pbuf + m->pParams[0].offset) = target;
			first = 1;
		}
		for (k = 0; cb->argKinds[k] != 0; k++) {
			PutArg(pbuf + m->pParams[first + k].offset, cb->argKinds[k], &args[k]);
		}
		pThread = Thread();
		Thread_SetEntryPoint(pThread, m->pMetaData, m->tableIndex, pbuf, m->parameterStackSize);
		Thread_ExecuteNested(pThread);
		switch (cb->retKind) {
		case 'i': result.i = *(int32_t*)Thread_LastReturn; break;
		case 'l': result.l = *(int64_t*)Thread_LastReturn; break;
		case 'f': result.f = *(float*)Thread_LastReturn; break;
		case 'd': result.d = *(double*)Thread_LastReturn; break;
		case 'p': result.p = (void*)*(intptr_t*)Thread_LastReturn; break;
		default: break;
		}
		if (pbuf != small) {
			free(pbuf);
		}
		d = (HEAP_PTR)pNext;
	}
	return result;
}

const char* FFI_CheckCallback(const tFFICallback *cb, tMD_TypeDef *pType) {
	static char msg[400];
	U32 i, n = 0;
	tMD_MethodDef *pInvoke = NULL;
	MetaData_Fill_TypeDef(pType, NULL, NULL);
	// the delegate's Invoke method
	for (i = 0; i < pType->numMethods; i++) {
		tMD_MethodDef *pM = pType->ppMethods[i];
		if (pM != NULL && strcmp((const char*)pM->name, "Invoke") == 0) { pInvoke = pM; break; }
	}
	if (pInvoke == NULL) {
		snprintf(msg, sizeof(msg), "%s has no Invoke method (it is not a delegate)", pType->name);
		return msg;
	}
	if (!pInvoke->isFilled) {
		MetaData_Fill_MethodDef(pType, pInvoke, NULL, NULL);
	}
	for (n = 0; cb->argKinds[n] != 0; n++) { }
	if (pInvoke->numberOfParameters != n + 1) {                      // (Invoke is an instance method: 'this' is the first)
		snprintf(msg, sizeof(msg), "the delegate %s takes %d argument(s), the callback type %s has %d", pType->name, (int)pInvoke->numberOfParameters - 1, cb->name, (int)n);
		return msg;
	}
	for (i = 0; i < n; i++) {
		char k = FFI_KindOf(pInvoke->pParams[i + 1].pTypeDef);
		if (k != cb->argKinds[i]) {
			snprintf(msg, sizeof(msg), "the delegate %s: argument %d is '%s' (kind %c), the callback type %s says kind %c", pType->name, (int)i + 1,
				pInvoke->pParams[i + 1].pTypeDef->name, k ? k : '?', cb->name, cb->argKinds[i]);
			return msg;
		}
	}
	{
		char rk = (pInvoke->pReturnType == NULL || pInvoke->pReturnType == types[TYPE_SYSTEM_VOID]) ? 'v' : FFI_KindOf(pInvoke->pReturnType);
		if (rk != cb->retKind) {
			snprintf(msg, sizeof(msg), "the delegate %s returns kind %c, the callback type %s says %c", pType->name, rk ? rk : '?', cb->name, cb->retKind);
			return msg;
		}
	}
	return NULL;
}
