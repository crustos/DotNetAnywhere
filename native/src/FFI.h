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

// NULL if the C# declaration of the method agrees with the entry's signature, else what disagrees
const char* FFI_CheckSignature(const tFFIEntry *pEntry, tMD_MethodDef *pMethod);

#endif
