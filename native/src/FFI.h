// Native FFI for ahead-of-time builds: the C functions that a program may call with [DllImport] are named in a manifest given to
// build.py (--ffi), which compiles their C files into the runtime and generates the table below. A DllImport is then looked up in the
// table when the method that calls it is compiled (no dlopen, no names at run time), and the call becomes one op that calls a wrapper
// directly on the evaluation stack, or, in a native block, a few stencils that call the function itself.

#if !defined (__FFI_H)
#define __FFI_H

#include "Types.h"
#include "MetaData.h"

// Reads the arguments where they lie on the evaluation stack (in the order of the call, each in its stack slot) and writes the result
// where the first argument was.
typedef void (*fnFFIWrapper)(PTR sp);

// The layout of a C struct that a call passes or returns by value, or points to (an array of them, or a ref): generated from the real C type
// (sizeof, offsetof), and checked against the C# struct when the method that calls is compiled, because a C# struct and a C struct that
// disagree in size or in where a field is would corrupt memory without a word.
typedef struct tFFIField_ {
	const char *name;
	U32 offset;                 // offsetof
	char kind;                  // i (a 32-bit integer or smaller), l (64-bit), f, d, p (a pointer)
	U32 size;                   // sizeof the field
} tFFIField;
typedef struct tFFIStruct_ {
	const char *name;
	U32 size;                   // sizeof
	U32 numFields;
	const tFFIField *fields;    // in the order of declaration
} tFFIStruct;

// A C function pointer parameter that a managed delegate stands behind ("callback:Name" in the manifest).  C function pointers carry no context, so the
// generated code has a fixed pool of trampolines for each callback type: a call that is passed a delegate takes a free one, which calls the delegate, and
// gives it back when the call returns.  A callback is valid only while the call it was passed to runs (a comparator, a visitor): there is nothing for C
// to keep, so nothing can dangle.
typedef union tFFIValue_ { int32_t i; int64_t l; float f; double d; void *p; } tFFIValue;
typedef struct tFFICallback_ {
	const char *name;
	const char *argKinds;       // i l f d p, one for each argument the C code calls it with
	char retKind;               // i l f d p, or v
	U32 slots;                  // trampolines in the pool
	HEAP_PTR *delegates;        // the delegate each slot calls (NULL: free)
	int64_t *handles;           // the host handle that keeps it (and its target) alive while C may call it
	void *const *trampolines;   // the C functions
} tFFICallback;

typedef struct tFFIEntry_ {
	const char *library;        // the name given to DllImport, without a path or suffix
	const char *entry;          // the function's name (EntryPoint)
	void *fn;                   // the function
	fnFFIWrapper wrapper;       // calls it on the evaluation stack
	U32 argBytes, retBytes;     // the arguments' and the result's size on the evaluation stack
	const char *args;           // one letter for each argument: i (a 32-bit integer or smaller), l (64-bit), f, d, p (a pointer-sized value: IntPtr, ref,
	                            // out, a pointer), b (an array: C gets a pointer to its elements), s (a string: C gets a temporary UTF-8 copy)
	char ret;                   // the same (s: a char* that is made a string and freed), or v for void
	const char *stencil;        // the name of the stencil that calls it from a native block (found by NativeBlock_FindStencil), or NULL
	const tFFIStruct *const *argStructs;   // for each argument: the layout of the struct it is (S), or an array of (b) or a ref to (p) one; NULL for any other. Or NULL
	const tFFIStruct *retStruct;           // the struct a result is (kind S), else NULL
	const tFFICallback *const *argCallbacks;   // for each argument: the callback type it is (kind c), else NULL. Or NULL
} tFFIEntry;

// The generated table (FFI.gen.c) or, when there is none, an empty one
extern const tFFIEntry ffiTable[];
extern const U32 ffiCount;

U32 FFI_Count(void);
const tFFIEntry* FFI_Find(const char *library, const char *entry);
// A string as a NUL-terminated UTF-8 copy: in `buf` (of `cap` bytes) if it fits, else malloc'd (and *pHeap is set: the caller frees it). NULL for null.
const char* FFI_StringToUtf8(HEAP_PTR pString, char *buf, size_t cap, int *pHeap);
// A new string from NUL-terminated UTF-8 (a bad sequence becomes U+FFFD)
HEAP_PTR FFI_StringFromUtf8(const char *pUtf8);

// The letter (as in tFFIEntry.args) for a C# type: i l f d p s b, v for void, or 0 if it cannot be passed to a C function directly
char FFI_KindOf(tMD_TypeDef *pType);
// A trampoline of `cb` for `delegate`, for the duration of one call: its index, to give back with FFI_CallbackRelease.  A null delegate is -1 (C gets NULL).
int FFI_CallbackAcquire(const tFFICallback *cb, HEAP_PTR delegate);
void FFI_CallbackRelease(const tFFICallback *cb, int slot);
// What a trampoline does: call the delegate of slot `slot` with the arguments C gave (as cb->argKinds says), and return what it returned
tFFIValue FFI_CallbackInvoke(const tFFICallback *cb, U32 slot, const tFFIValue *args);
// NULL if the delegate type `pType` has the signature the manifest declares for `cb`, else what differs (a static message)
const char* FFI_CheckCallback(const tFFICallback *cb, tMD_TypeDef *pType);

// NULL if the C# struct `pType` agrees with the C struct: the same size, the same number of fields, and each at the same offset, of the same size and kind;
// else what disagrees (a static message)
const char* FFI_CheckStruct(const tFFIStruct *pC, tMD_TypeDef *pType);

// NULL if the C# declaration of the method agrees with the entry's signature, else what disagrees
const char* FFI_CheckSignature(const tFFIEntry *pEntry, tMD_MethodDef *pMethod);

#endif
