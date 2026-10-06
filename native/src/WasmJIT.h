// WasmJIT: compile a method's CIL to a WebAssembly function at run time (wasm32 builds only).
//
// How it fits in: JIT_Prepare (JIT.c) offers each method to WasmJIT_Compile before the interpreter's own translation. If the method is
// inside the supported subset (see WasmJIT.c), it comes back as an index in the indirect function table, and the method's instruction
// stream becomes   [JIT_WASM_METHOD][index][size of the return value][tWasmDeopt*][JIT_RETURN]   so nothing about calling it changes: the
// interpreter still pushes the arguments, and the instruction runs the wasm function on that frame and leaves the result on the
// evaluation stack. Anything outside the subset is left to the interpreter.
//
// The compiled function has the signature  U32 f(PTR frame, PTR evalStackTop, U32 entry, U32 budget):  it reads the arguments from the
// frame, writes the return value at evalStackTop, and returns WJ_OK, or a status for the interpreter to turn into an exception.
//
// A loop does not run for ever in one call. Every backward branch costs one of `budget`; when it is spent the function writes its arguments
// and locals back to the frame and returns WJ_RESTART + entry, and the interpreter (which has thereby given the other threads their turn)
// calls it again with that entry, which resumes at the loop. That is how DNA's native blocks share the processor too, and it has a second
// use here: an engine can only move a wasm function to a better compiler between calls, so a kernel that is called once and loops for a
// long time would otherwise run its baseline-compiled code from start to finish.
//
// The host must provide the import dna.emit_wasm (tools/run_wasm.mjs does) and the module must export its indirect function
// table and its memory (build.py --wasm links with --export-table --growable-table). DNA_NO_WASM_JIT=1 turns the compiler off at run time,
// build.py --wasm --no-wasm-jit leaves it out (and then no host import is needed); DNA_WASM_JIT_DEBUG=1 says what happened to each method,
// DNA_WASM_JIT_DUMP=dir writes each module to dir/m<method>.wasm, DNA_WASM_JIT_LIMIT=n compiles only the first n qualifying methods (bisection), DNA_WASM_JIT_MIN=n sets how many CIL instructions a method without a
// loop needs to be compiled (default 12).
#if !defined(__WASMJIT_H)
#define __WASMJIT_H

#include "Types.h"

#if defined(__wasm__) && !defined(DNA_NO_WASM_JIT_BUILD)
#define WASM_JIT 1
#else
#define WASM_JIT 0
#endif

// statuses of a compiled function
#define WJ_OK 0
#define WJ_NULLREF 1
#define WJ_INDEXRANGE 2
#define WJ_DIVZERO 3
#define WJ_OVERFLOW 4
#define WJ_DEOPT 0x10000      // WJ_DEOPT + n: a guard failed at deoptimization site n: the interpreter takes over (see tWasmDeopt)
#define WJ_RESTART 0x100      // WJ_RESTART + n: out of budget, call again with entry n
#define WJ_SLICE 20000        // the budget a call is given: how many backward branches before it returns to the interpreter

// The budget each call is given (DNA_WASM_JIT_SLICE=n overrides WJ_SLICE: 1 makes every backward branch give up the processor and resume,
// which is how the tests check that nothing is lost on the way)
U32 WasmJIT_Slice(void);

typedef U32 (*tWasmFn)(PTR frame, PTR evalStackTop, U32 entry, U32 budget);

// Speculative devirtualization (WasmJIT.c): a virtual or interface call is compiled for the targets that the types that exist now can reach, with a
// guard on the receiver. If the guard fails (a type that did not exist at compile time, or one not predicted), the function writes its state in
// the interpreter's layout -- locals in the frame, the evaluation stack's contents on the evaluation stack -- and returns WJ_DEOPT + site; the
// interpreter then goes on with the same method from the call instruction, using its own version of the method. That is exactly what running it
// there would have done: exceptions, blocking, recursion and the collector all behave as they always did.
// where in the interpreter's ops to go on; how much stack was written; the method that was called virtually, and where its receiver is among that stack
typedef struct tWasmDeoptSite_ { U32 ip; U32 stackBytes; tMD_MethodDef *callee; U32 recvOfs; } tWasmDeoptSite;
typedef struct tWasmOsr_ { U32 cilOfs, entry; } tWasmOsr;                      // a loop header: the entry value that starts the compiled version there
// The interpreter's version has an op at each outermost loop header (JIT_WASM_OSR) that takes the frame back into the compiled code, there.
// A frame of a method that the compiled code called, in the middle of the chain of calls that a guard failure unwinds: the callee that failed writes an
// image of its frame in the interpreter's layout -- [parameters][locals][evaluation stack] -- and each caller below it writes one of its own, at its call.
// The images are pushed on WasmJIT_ChainDesc/Image, innermost first; the entry function then returns WJ_DEOPT + site as for a failure of its own, and the
// interpreter makes a frame from each image (the entry's frame is the one that exists), with the callee's return going on into the caller as it always did.
typedef struct tWasmFrameDesc_ {
	tMD_MethodDef *method;
	U32 cilOfs, ip;              // where the interpreter goes on in `interp`: the CIL offset, and (when compilation is done) the op
	U32 plBytes, stackBytes;     // the sizes of the parameters and locals, and of the evaluation stack, in the image
	tMD_MethodDef *callee; U32 recvOfs;   // the innermost frame: the method that was called virtually, and where its receiver is on the stack
	U32 numReloc; U32 *reloc;    // pointers on the stack to a struct in the frame's locals: [offset in the stack, offset in the frame] pairs, to be made addresses in the new frame
	tJITted *interp;
} tWasmFrameDesc;
#define WJ_MAX_CHAIN 64
#define WJ_CHAIN 0x7000          // the module's status global while a failure of a guard unwinds the compiled functions
extern U32 WasmJIT_ChainLen;
extern const tWasmFrameDesc *WasmJIT_ChainDesc[WJ_MAX_CHAIN];
extern PTR WasmJIT_ChainImage[WJ_MAX_CHAIN];

// What it takes to compile the method again, with the receiver types seen to fail the guards (JIT_RecompileWasm).
typedef struct tWasmDeopt_ {
	tJITted *interp; U32 numSites; tWasmDeoptSite *sites; U32 numOsr; tWasmOsr *osr;
	const U8 *cil; U32 codeSize; tParameter *locals; U32 numLocals, origLocalsSize, recompiles;
} tWasmDeopt;
// Room that a method with guards has in its frame beyond what it needs, so that a version that has more targets (and so may need more scratch) can take over
// the frames of the one before it, which are sized from the method's current version.
#define WJ_DEOPT_SLACK 256
// What compiling a method gives besides the function.
typedef struct tWasmResult_ { U32 extraFrame; U32 maxStack; tWasmDeopt *deopt; } tWasmResult;

#if WASM_JIT
// (the types are declared by the includes of the file that includes this; the order of those headers matters)
int WasmJIT_Enabled(void);
// The table index of the compiled method, or 0 if it was not compiled. pCIL/codeSize: the method body, pLocals/numLocals: its locals.
// origLocalsSize: the size of the frame's locals as the interpreter laid them out; *pExtraFrame gets how much more frame the function needs
// (the scratch area, see WasmJIT.c), which the caller adds to the method's locals.
U32 WasmJIT_Compile(tMD_MethodDef *pMethodDef, const U8 *pCIL, U32 codeSize, const tParameter *pLocals, U32 numLocals, U32 origLocalsSize, tWasmResult *pResult);
void JIT_BuildInterpreterVersion(tMD_MethodDef *pMethodDef, U8 *pCIL, U32 codeSize, tParameter *pLocals, U32 headerMaxStack, U32 localsStackSize,
		const U32 *keep, U32 numKeep, const U32 *osr, U32 numOsr, tJITted **ppOut, U32 **ppCilToOp);
// Whether the compiled version of a method can be entered at the loop whose header is at this CIL offset, and with what entry value.
int WasmJIT_OsrEntry(const tJITted *pCompiled, U32 cilOfs, U32 *pEntry);
// A receiver of this type failed the guards of the call of `callee` in this method: 1 if that is new (and the method should be compiled again with it).
int WasmJIT_NoteMiss(tMD_MethodDef *pMethod, tMD_MethodDef *callee, tMD_TypeDef *type);
int JIT_RecompileWasm(tMD_MethodDef *pMethodDef);
#endif

#endif
