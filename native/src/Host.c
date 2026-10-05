// See Host.h

#include <stdarg.h>

#include "Compat.h"
#include "Sys.h"

#include "Host.h"
#include "CLIFile.h"
#include "MetaData.h"
#include "MetaDataTables.h"
#include "Type.h"
#include "Heap.h"
#include "Finalizer.h"
#include "System.Net.Sockets.Socket.h"
#include "JIT.h"
#include "Thread.h"
#include "FFI.h"
#include "System.String.h"
#include "System.Array.h"
#include "EvalStack.h"

static int initialised = 0;
static int depth = 0;                      // how many managed executions are running: > 0 means a call from native code that managed code called
static char errbuf[400];
static char *retString = NULL;             // the last string result, as UTF-8
static void *retArray = NULL;              // the last array result

const char* DNA_Error(void) {
	return errbuf;
}

static int Fail(const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(errbuf, sizeof(errbuf), fmt, ap);
	va_end(ap);
	return -1;
}

// ---- handles: how native code holds a managed object ---------------------------------------------------------------------------------
//
// A handle is the number of a slot (1, 2, ...) in a table of object pointers.  The table is a GC root (Host_GetHeapRoots, called by every
// collection), so an object stays alive, with everything it refers to, while any slot holds it.  A free slot holds the number of the next free
// one, which is far too small to be taken for a pointer.  Nothing here allocates on the managed heap, so no collection can happen between
// the moment a method returns an object and the moment it is put in a slot.
static void **hTable = NULL;
static unsigned char *hUsed = NULL;
static U32 hCap = 0;
static U32 hFree = 0;                      // the first free slot's handle, or 0
static int hLive = 0;

static int64_t NewHandle(HEAP_PTR p) {
	U32 slot;
	if (p == NULL) {
		return 0;
	}
	if (hFree == 0) {
		U32 i, newCap = hCap ? hCap * 2 : 64;
		hTable = (void**)realloc(hTable, newCap * sizeof(void*));
		hUsed = (unsigned char*)realloc(hUsed, newCap);
		for (i = hCap; i < newCap; i++) {
			hTable[i] = (void*)(uintptr_t)(i + 2 <= newCap ? i + 2 : 0);      // the next free handle (slot i is handle i + 1)
			hUsed[i] = 0;
		}
		hFree = hCap + 1;
		hCap = newCap;
	}
	slot = hFree - 1;
	hFree = (U32)(uintptr_t)hTable[slot];
	hTable[slot] = p;
	hUsed[slot] = 1;
	hLive++;
	return (int64_t)slot + 1;
}

static int HandleValid(int64_t h) {
	return h == 0 || (h > 0 && (uint64_t)h <= hCap && hUsed[h - 1]);
}

static HEAP_PTR HandleGet(int64_t h) {
	return h == 0 ? NULL : (HEAP_PTR)hTable[h - 1];
}

int64_t Host_Pin(void *heapPtr) {
	return NewHandle((HEAP_PTR)heapPtr);
}

int DNA_Release(int64_t h) {
	if (h == 0) {
		return 0;
	}
	if (!HandleValid(h)) {
		return Fail("handle %lld is not live (never given out, or released already)", (long long)h);
	}
	hTable[h - 1] = (void*)(uintptr_t)hFree;
	hUsed[h - 1] = 0;
	hFree = (U32)h;
	hLive--;
	return 0;
}

int DNA_LiveHandles(void) {
	return hLive;
}

void Host_GetHeapRoots(tHeapRoots *pHeapRoots) {
	if (hCap > 0) {
		Heap_SetRoots(pHeapRoots, hTable, hCap * (U32)sizeof(void*));
	}
}

// The letter for a C# type in a call from native code: FFI_KindOf's, and 'o' for an object (a class: not a string, not an array)
static char HostKind(tMD_TypeDef *pType) {
	char k = FFI_KindOf(pType);
	if (k == 'S' || k == 'c') {
		return 0;                  // (a struct or a delegate cannot be passed between native code and a managed method through DNA_Call: only through a [DllImport])
	}
	if (k != 0) {
		return k;
	}
	MetaData_Fill_TypeDef(pType, NULL, NULL);
	if (pType->stackType == EVALSTACK_O && !TYPE_ISARRAY(pType) && pType != types[TYPE_SYSTEM_STRING]) {
		return 'o';
	}
	return 0;
}

void DNA_SetCrashMode(int abortLikeDotNet) {
	Crash_AbortMode = abortLikeDotNet;
}

void DNA_SetAssemblyDir(const char *dir) {
	CLIFile_SetAssemblyDir(dir);
}

void DNA_SetAssemblyDirFromFile(const char *path) {
	char dir[1024];
	const char *slash = strrchr(path, '/');
	if (slash == NULL) {
		CLIFile_SetAssemblyDir(".");
	} else if ((size_t)(slash - path) < sizeof(dir)) {
		memcpy(dir, path, (size_t)(slash - path));
		dir[slash - path] = 0;
		CLIFile_SetAssemblyDir(dir[0] ? dir : "/");
	}
}

void DNA_Init(void) {
	if (initialised) {
		return;
	}
	initialised = 1;
	errbuf[0] = 0;
	JIT_Execute_Init();
	MetaData_Init();
	Type_Init();
	Heap_Init();
	Finalizer_Init();
	Socket_Init();
}

DNA_Assembly* DNA_Load(const char *path) {
	FILE *f;
	if (!initialised) {
		Fail("DNA_Init has not been called");
		return NULL;
	}
	f = fopen(path, "rb");
	if (f == NULL) {
		Fail("cannot open %s", path);
		return NULL;
	}
	fclose(f);
	return CLIFile_Load((char*)path);
}

DNA_Assembly* DNA_Loaded(const char *assemblyName) {
	return CLIFile_FindLoaded(assemblyName);
}

// One letter per parameter, '>', the result: what DNA_Find's `sig` is compared with
static void Signature(tMD_MethodDef *m, char *out) {
	U32 i;
	for (i = 0; i < m->numberOfParameters; i++) {
		char k = HostKind(m->pParams[i].pTypeDef);
		out[i] = k ? k : '?';
	}
	out[i++] = '>';
	{
		char k = HostKind(m->pReturnType);
		out[i++] = k ? k : '?';
	}
	out[i] = 0;
}

DNA_Method* DNA_Find(DNA_Assembly *a, const char *ns, const char *cls, const char *method, const char *sig) {
	tMD_TypeDef *t;
	tMD_MethodDef *found = NULL;
	U32 i;
	int matches = 0;

	if (a == NULL) {
		Fail("no assembly");
		return NULL;
	}
	t = MetaData_GetTypeDefFromName(a->pMetaData, (STRING)ns, (STRING)cls, NULL, 0);
	if (t == NULL) {
		Fail("no class %s%s%s", ns, ns[0] ? "." : "", cls);
		return NULL;
	}
	MetaData_Fill_TypeDef(t, NULL, NULL);
	for (i = 0; i < t->numMethods; i++) {
		tMD_MethodDef *m = t->ppMethods[i];
		char have[64];
		if (strcmp((const char*)m->name, method) != 0 || !METHOD_ISSTATIC(m)) {
			continue;
		}
		if (!m->isFilled) {
			MetaData_Fill_MethodDef(t, m, NULL, NULL);
		}
		if (m->numberOfParameters > 60) {
			continue;
		}
		Signature(m, have);
		if (sig != NULL && strcmp(have, sig) != 0) {
			continue;
		}
		if (found == NULL) {
			found = m;
		}
		matches++;
	}
	if (found == NULL) {
		Fail("no static method %s.%s%s%s%s in the assembly", cls, method, sig ? " with signature " : "", sig ? sig : "", "");
		return NULL;
	}
	if (matches > 1) {
		Fail("%s.%s is overloaded: give the signature", cls, method);
		return NULL;
	}
	return found;
}

static int Blittable(tMD_TypeDef *pElem) {
	MetaData_Fill_TypeDef(pElem, NULL, NULL);
	switch (pElem->stackType) {
	case EVALSTACK_INT32: case EVALSTACK_INT64: case EVALSTACK_F32: case EVALSTACK_F64: case EVALSTACK_INTNATIVE:
		return 1;
	default:
		return 0;
	}
}

int DNA_Call(DNA_Method *m, const DNA_Value *args, int nargs, DNA_Value *ret) {
	U8 small[128];
	U8 *pbuf = small;
	HEAP_PTR held[16];                      // the strings and arrays made for the arguments: kept alive until the call is done
	tThread *pThread;
	char rk;
	int i;

	if (!initialised) {
		return Fail("DNA_Init has not been called");
	}
	if (m == NULL) {
		return Fail("no method");
	}
	if (!METHOD_ISSTATIC(m)) {
		return Fail("%s is not static", m->name);
	}
	if (nargs != (int)m->numberOfParameters || nargs > 16) {
		return Fail("%s takes %d argument(s), %d given", m->name, (int)m->numberOfParameters, nargs);
	}
	// everything is checked before anything is allocated
	for (i = 0; i < nargs; i++) {
		tMD_TypeDef *pt = m->pParams[i].pTypeDef;
		char want = HostKind(pt);
		if (want == 0) {
			return Fail("%s: parameter %d (%s) cannot be passed from native code", m->name, i + 1, pt->name);
		}
		if (args[i].kind != want) {
			return Fail("%s: parameter %d is kind '%c' in C#, but '%c' was passed", m->name, i + 1, want, args[i].kind);
		}
		if (want == 'o' && !HandleValid(args[i].u.l)) {
			return Fail("%s: parameter %d: handle %lld is not live (never given out, or released already)", m->name, i + 1, (long long)args[i].u.l);
		}
		if (want == 'b') {
			tMD_TypeDef *pe = pt->pArrayElementType;
			if (pe == NULL || !Blittable(pe)) {
				return Fail("%s: parameter %d is an array of %s, which is not blittable", m->name, i + 1, pe ? (const char*)pe->name : "?");
			}
			if (args[i].elemSize != (int)pe->arrayElementSize) {
				return Fail("%s: parameter %d is an array of %d-byte elements in C#, but %d-byte elements were passed", m->name, i + 1,
					(int)pe->arrayElementSize, (int)args[i].elemSize);
			}
			if (args[i].len < 0 || (args[i].len > 0 && args[i].u.data == NULL)) {
				return Fail("%s: parameter %d: an array of %d element(s) with no data", m->name, i + 1, (int)args[i].len);
			}
		}
	}
	rk = HostKind(m->pReturnType);
	if (rk == 0) {
		return Fail("%s: the result type cannot be returned to native code", m->name);
	}
	if (rk == 'b' && !Blittable(m->pReturnType->pArrayElementType)) {
		return Fail("%s: the result is an array that is not blittable", m->name);
	}

	free(retString); retString = NULL;
	free(retArray); retArray = NULL;

	if (m->parameterStackSize > sizeof(small)) {
		pbuf = (U8*)malloc(m->parameterStackSize);
	}
	memset(pbuf, 0, m->parameterStackSize);
	for (i = 0; i < nargs; i++) {
		U8 *at = pbuf + m->pParams[i].offset;
		held[i] = NULL;
		switch (args[i].kind) {
		case 'i': *(int32_t*)at = args[i].u.i; break;
		case 'l': *(int64_t*)at = args[i].u.l; break;
		case 'f': *(float*)at = args[i].u.f; break;
		case 'd': *(double*)at = args[i].u.d; break;
		case 'p': *(intptr_t*)at = args[i].u.p; break;
		case 's':
			if (args[i].u.s != NULL) {
				held[i] = FFI_StringFromUtf8(args[i].u.s);
				Heap_MakeUndeletable(held[i]);
			}
			*(HEAP_PTR*)at = held[i];
			break;
		case 'o':
			*(HEAP_PTR*)at = HandleGet(args[i].u.l);        // (alive: the table is a root)
			break;
		case 'b': {
			tMD_TypeDef *pt = m->pParams[i].pTypeDef;
			MetaData_Fill_TypeDef(pt, NULL, NULL);
			held[i] = SystemArray_NewVector(pt, (U32)args[i].len);
			Heap_MakeUndeletable(held[i]);
			if (args[i].len > 0) {
				memcpy(SystemArray_GetElements(held[i]), args[i].u.data, (size_t)args[i].len * (size_t)args[i].elemSize);
			}
			*(HEAP_PTR*)at = held[i];
			break;
		}
		}
	}

	pThread = Thread();
	Thread_SetEntryPoint(pThread, m->pMetaData, m->tableIndex, pbuf, m->parameterStackSize);
	if (depth > 0) {
		Thread_ExecuteNested(pThread);
	} else {
		depth++;
		Thread_Execute();
		depth--;
	}

	// The result, before anything else can allocate (and collect)
	if (ret != NULL) {
		memset(ret, 0, sizeof(*ret));
		ret->kind = rk;
		switch (rk) {
		case 'i': ret->u.i = *(int32_t*)Thread_LastReturn; break;
		case 'l': ret->u.l = *(int64_t*)Thread_LastReturn; break;
		case 'f': ret->u.f = *(float*)Thread_LastReturn; break;
		case 'd': ret->u.d = *(double*)Thread_LastReturn; break;
		case 'p': ret->u.p = *(intptr_t*)Thread_LastReturn; break;
		case 'o': ret->u.l = NewHandle(*(HEAP_PTR*)Thread_LastReturn); break;
		case 's': {
			HEAP_PTR s = *(HEAP_PTR*)Thread_LastReturn;
			if (s != NULL) {
				int heap;
				retString = (char*)FFI_StringToUtf8(s, NULL, 0, &heap);      // (cap 0: always a malloc'd copy)
			}
			ret->u.s = retString;
			break;
		}
		case 'b': {
			HEAP_PTR arr = *(HEAP_PTR*)Thread_LastReturn;
			if (arr != NULL) {
				U32 es = Heap_GetType(arr)->pArrayElementType->arrayElementSize;
				U32 n = SystemArray_GetLength(arr);
				retArray = malloc((size_t)n * es + 1);
				memcpy(retArray, SystemArray_GetElements(arr), (size_t)n * es);
				ret->len = (int32_t)n;
				ret->elemSize = (int32_t)es;
			}
			ret->u.data = retArray;
			break;
		}
		}
	}

	// arrays: what the callee wrote is visible to the caller; then let the collector have them all again
	for (i = 0; i < nargs; i++) {
		if (args[i].kind == 'b' && held[i] != NULL && args[i].len > 0) {
			memcpy(args[i].u.data, SystemArray_GetElements(held[i]), (size_t)args[i].len * (size_t)args[i].elemSize);
		}
		if (held[i] != NULL) {
			Heap_MakeDeletable(held[i]);
		}
	}
	if (pbuf != small) {
		free(pbuf);
	}
	return 0;
}

int DNA_RunMain(DNA_Assembly *a, int argc, char **argv) {
	int r = 0;
	if (a == NULL || !a->entryPoint) {
		return 0;
	}
	depth++;
	r = (int)CLIFile_Execute(a, argc, argv);
	depth--;
	return r;
}
