// Host: DotNetAnywhere as a library inside a native program.
//
// dna.c is a program: it loads an assembly and runs its entry point.  This is the other way round: a C program (here, what CC# lowers
// through Crust) starts the runtime, loads an assembly of C# that Crust could not lower, and calls its static methods.
//
//     DNA_Init();
//     DNA_Assembly *a = DNA_Load("managed.dll");                       // corlib.dll must sit beside it
//     DNA_Method *m = DNA_Find(a, "Game", "Script", "Score", "il>i");   // namespace, class, method, signature (or NULL for any)
//     DNA_Value arg[2] = { DNA_Int(3), DNA_Str("hi") }, ret;
//     if (DNA_Call(m, arg, 2, &ret) == 0) printf("%d\n", ret.u.i);
//
// What can cross is what C can hold, and what a [DllImport] can take in tools/gen_ffi.py (the same letters):
//
//     i   int32, and everything that is one on the evaluation stack: bool, byte, sbyte, short, ushort, char, int, uint, an enum
//     l   int64, uint64      f   float      d   double      p   a pointer-sized value (IntPtr)
//     s   string.  In: a NUL-terminated UTF-8 string (copied).  Out: UTF-8 in a buffer that stays valid until the next DNA_Call.
//     b   an array of a blittable type.  In: copied in, and copied back after the call, so the callee's writes are seen (the length
//         cannot change).  Out: a copy, valid until the next DNA_Call.  `elemSize` must be that of the method's element type.
//     o   an object (a class instance), as a HANDLE: an int64 that stands for it.  A handle is the way native code holds a managed object: the
//         runtime keeps what it stands for alive (and everything that object refers to) until DNA_Release.  Handle 0 is null.  In: the method's
//         parameter is a class, and the object is the one the handle stands for.  Out: a new handle for the object the method returned, which the
//         caller owns and must release (a method that returns an object it already returned gives another handle: they are independent).
//     v   (a result only) void
//
// Only static methods.  A managed exception that nothing catches ends the process (DNA's Crash), as an uncaught exception does in a
// .NET program and in Crust.
//
// A call may come from the host (a native `main`, or a native function called by native code), or from a native function that managed
// code called through [DllImport] (the --ffi build): a *nested* call.  The managed code of a nested call must not block (Sleep, a lock).

#if !defined (__HOST_H)
#define __HOST_H

#include <stdint.h>

typedef struct tCLIFile_ DNA_Assembly;
typedef struct tMD_MethodDef_ DNA_Method;

typedef struct DNA_Value {
	char kind;                 // i l f d p s b v
	union {
		int32_t i;
		int64_t l;
		float f;
		double d;
		intptr_t p;
		const char *s;
		void *data;
	} u;
	int32_t len;               // b: the number of elements
	int32_t elemSize;          // b: bytes per element
} DNA_Value;

#define DNA_Int(v)    ((DNA_Value){ 'i', { .i = (int32_t)(v) }, 0, 0 })
#define DNA_Long(v)   ((DNA_Value){ 'l', { .l = (int64_t)(v) }, 0, 0 })
#define DNA_Float(v)  ((DNA_Value){ 'f', { .f = (float)(v) }, 0, 0 })
#define DNA_Double(v) ((DNA_Value){ 'd', { .d = (double)(v) }, 0, 0 })
#define DNA_Ptr(v)    ((DNA_Value){ 'p', { .p = (intptr_t)(v) }, 0, 0 })
#define DNA_Str(v)    ((DNA_Value){ 's', { .s = (v) }, 0, 0 })
#define DNA_Obj(h)    ((DNA_Value){ 'o', { .l = (int64_t)(h) }, 0, 0 })
#define DNA_Array(ptr, n, size) ((DNA_Value){ 'b', { .data = (ptr) }, (int32_t)(n), (int32_t)(size) })

// Where corlib.dll (and any other assembly a program refers to) is looked for first; the current directory is the fallback.  Call it BEFORE
// DNA_Init: corlib.dll is loaded while the runtime starts.  Without it, a program only works when started in the directory that holds corlib.dll.
void DNA_SetAssemblyDir(const char *dir);
// The same, with the directory of `path` (a file name with no directory means the current one)
void DNA_SetAssemblyDirFromFile(const char *path);

// How a failure that cannot be recovered from ends the process: an exception that nothing catches, a corrupt assembly.  0 (the default, and
// what `dna` does): the report on stdout and exit status 1.  1: as an unhandled exception ends a .NET program (and Crust): what was printed
// so far is flushed, the report goes to stderr, and the process is killed by SIGABRT.
void DNA_SetCrashMode(int abortLikeDotNet);

// Start the runtime.  Safe to call more than once.
void DNA_Init(void);

// Load an assembly (and, beside it, corlib.dll).  NULL, with the reason in DNA_Error(), if there is no such file.
DNA_Assembly* DNA_Load(const char *path);

// An assembly that is loaded already (the one a program that is itself running managed code, calling C through [DllImport], is in), by
// its assembly name ("Managed" for Managed.dll).  NULL if there is none.
DNA_Assembly* DNA_Loaded(const char *assemblyName);

// The static method `cls.method` of `ns` in `a`.  `sig` selects an overload and checks the types: one letter for each parameter, '>', then
// the result: "il>d" is double M(int, long).  NULL takes the only method of that name.  NULL, with the reason in DNA_Error(), if none.
DNA_Method* DNA_Find(DNA_Assembly *a, const char *ns, const char *cls, const char *method, const char *sig);

// Call it.  0 on success; else DNA_Error() says why (a wrong number or kind of argument; nothing has run).  `ret` may be NULL.
int DNA_Call(DNA_Method *m, const DNA_Value *args, int nargs, DNA_Value *ret);

// Run the assembly's entry point (its `Main`), as dna.c does.  argv[0] is the program's name.  Returns Main's int result, or 0.
int DNA_RunMain(DNA_Assembly *a, int argc, char **argv);

// Let go of a handle: the object it stood for may now be collected, if nothing else refers to it.  Releasing 0 does nothing; releasing a handle
// twice, or one that was never given out, is refused (DNA_Error says so) and changes nothing.
int DNA_Release(int64_t handle);

// How many handles are live (for tests, and for a host that wants to check it does not leak)
int DNA_LiveHandles(void);
// For the runtime itself (FFI.c): a handle for a managed object, to release with DNA_Release.  An object that has one stays alive, and so does what it refers to.
int64_t Host_Pin(void *heapPtr);

const char* DNA_Error(void);

#endif
