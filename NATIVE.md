# Native build, Crust integration and test suite

This fork builds DotNetAnywhere natively with gcc (the original build is Emscripten-only), moves part
of the runtime into the [Crust](https://github.com/brentharts/crust) C++ subset, extends corlib with
what Crust's C# subset needs (`Marshal`, `MemoryMarshal`, `Span<T>`, `[StructLayout]`), and fixes a
large number of runtime bugs found by comparing DNA against Mono.

## Building and testing

`build.py` is the only build script (the old CMake file, `build.sh` and `build.cmd` are gone), and `make` just calls
it. Needs: `gcc`, `python3`, a Crust checkout at `../crust` (or `--crust DIR` / `$CRUST_ROOT`), and, for corlib and
the tests, `mono-mcs`, `mono-runtime` and `mono-devel` (for `ilasm`). `gcc-multilib` is needed only for the 32-bit
build. The reference run in `MathBits` also uses a .NET SDK if one is installed.

    git clone https://github.com/brentharts/crust.git ../crust
    make                                  # python3 build.py: build/dna (native, 64-bit on x86-64) + build/corlib.dll
    make ARGS="--m32"                     # build/dna32 (32-bit)
    make test                             # the whole suite (--32 / --64 pick the binary; default build/dna)
    python3 tests/crust_conformance.py    # Crust's own C# programs, DNA vs Mono (same flags)

`native/src/MetaDataLayout.gen.h` is generated, not committed: `build.py` runs `tools/gen_metadata_layout.py` when it
is missing or older than the files it is generated from, and `--clean` removes it. The generator works out the
32-bit layout itself from the struct declarations and the typedefs in `Types.h`, so it needs no compiler;
`--verify-with-compiler` (which the test suite passes when a 32-bit gcc exists) cross-checks it against `gcc -m32`.

Run a program with `build/dna prog.exe` (or `dna32`), compiled with `mcs -nostdlib -r:build/corlib.dll prog.cs` and
with `corlib.dll` beside the exe. `--debug` gives `-O0 -g`.

Both word sizes pass the same suite, and the tests do not depend on the pointer size: they compare with Mono (or
.NET), not with stored output. Tested on Linux x86-64 with gcc only.

**WebAssembly is paused.** The Emscripten build is gone (`build.py --wasm` existed briefly and was removed too). The
intended route is `clang --target=wasm32`, and later a JIT that emits wasm. `js-interop.js` and the JSInterop /
Debugger entry points are left in the tree for that, but nothing builds or tests them now.

## What was added

* **`build.py`**: gcc build with parallel, cached compilation; lowers `native/src/cpp/*.cpp` with
  Crust's `cpprust` first. `NativeHost.c` stubs the JS bridge for native builds.
* **A runtime module in the Crust C++ subset**: the heap-tracking tree (`native/src/cpp/HeapTree.cpp`,
  class `AATree`) replaces the C code in `Heap.c`; a differential test pins it to the original.
* **corlib**: `Marshal`, `MemoryMarshal`, `Span<T>`/`ReadOnlySpan<T>`, `StructLayoutAttribute`,
  `LayoutKind`, `Unsafe.SizeOf`, `TypedReference`, `IsVolatile`, `DivideByZeroException`.
  Marshalling serialises unmanaged structs field by field in the reference runtime's Sequential
  layout (natural alignment, `Pack` honoured); it cannot be a `memcpy` because DNA keeps every small
  field in a 4-byte slot.
* **Opcodes**: `break`, `ckfinite`, `cpobj`, `cpblk`, `initblk`, `sizeof`, `calli`, `jmp`, `ldvirtftn`,
  `unbox`, `mkrefany`/`refanyval`/`refanytype`, the two-byte `ldarg`/`ldloc`/`stloc`/... forms, the
  `volatile.`/`tail.`/`unaligned.` prefixes, unsigned 64-bit branches, checked 64-bit arithmetic and
  every `conv.ovf.*`, `float`/`double` `%` and unary minus, unordered float compares, `stind.i8/r4/r8`,
  `ldind.i`, `ldelem.i`/`stelem.i`. The reserved prefix encodings are refused with a clear message.
* **Exception filters** (`catch ... when`) and `fault` blocks, via a real two-pass search.

## Runtime bugs fixed

Each has a regression test, and expectations were checked against Mono.

* Unwinding more than one frame crashed; `finally` blocks never ran while unwinding, or were skipped
  when the catch was in the same method; the GC scanned dead frames after a caught exception.
* Failed `castclass` threw a stale unrelated object (a shadowed `heapPtr` behind the `THROW` macro).
* `JIT_CONV_R64_I64` popped 4 bytes of a double; `float - float` used `double` operands.
* Widening conversions chose zero- or sign-extension from the variable's static type, not the opcode.
* Integer division by zero / `MinValue / -1` killed the process with SIGFPE.
* Loads and stores through a `ref` into `byte[]`/`short[]`/`char[]` used 4-byte accesses and clobbered
  neighbouring elements. Field access through a null object segfaulted.
* `unbox.any` did no checking; `System.ValueType` and `System.Enum` were value types of size 0.
* Enums: only `int` enums could be converted to text; undefined values and `[Flags]` were formatted
  wrongly; `GetNames` ignored value order.
* `List<T>.Remove(default)` deleted the last element; `RemoveAt` had no bounds check; `Type ==` was
  wrong for null; `FullName` printed `.Name` for the global namespace.
* An uninitialised variable in `JIT.c` (a `goto` past its initialiser) made `Dictionary` crash
  depending on stack contents; the type table truncated sizes above 255 bytes.
* `leave` chose the `finally` to run from an instruction position that is only saved at calls, so a `try` body with no call
  never ran its `finally`; and it ran only the innermost `finally`, so `return`/`break` out of nested `try/finally` blocks
  skipped the outer ones.
* A `catch` handler compiled before its exception type had been used had the exception reference left on the evaluation
  stack (its `pop` was sized from a type not yet filled in), which overflowed onto the method's parameters.
* Reading a `static long` field crashed the JIT ("Opcode not available"): the opcode had no handler.
* `stobj` popped the destination address as 4 bytes, so storing a struct into an array element corrupted the stack on a
  64-bit target.
* `Dictionary`: `Count` was one too low after the first resize, and growing it was quadratic (two `List`s per slot, and a
  collection every 200 KB of allocation whatever the heap size); both fixed, the class rewritten.
* Array access was not checked at all: reading or writing past the end of an array touched whatever memory was there, and a null
  array crashed the process. `ldelem.*`, `stelem.*`, `ldelema` and `ldlen` now throw `IndexOutOfRangeException` and
  `NullReferenceException`.
* Every ordered float or double comparison with a NaN operand came out true: `bge.un`, `bgt.un`, `ble.un` and `blt.un` (what C#
  compiles `if (a < b) body` to) shared the ordered handlers, so `if (NaN < 1f)` ran its body.
* `Thread.Sleep` in the main thread ended the program (rc = the sleep time): the scheduler returned when every thread was
  sleeping, which only a JavaScript host wants.
* **Delegates**: every invocation `malloc`'d a copy of its arguments and kept it on the *caller's* frame, so a loop calling a delegate
  N times lost N-1 buffers (53 MB peak on the `delegates` benchmark; now 11.5 MB, and 2.7x faster: a one-target delegate uses its
  arguments where they are). `d -= x` crashed ("Opcode not available": `Delegate.Equals` boxes an `IntPtr`, and boxing a pointer-typed
  value had no handler, on 32 bit as well), and behind that `RemoveImpl` removed *every* matching target where .NET removes the last.
* **Interface calls on `null`** crashed the process (the receiver was never checked); they throw `NullReferenceException`.
* **Casts of arrays**: `x is Array` was false for every array, and an `int[]` was an `object[]` (covariance was applied to value-type
  elements). `castclass`/`isinst` also accept an exact type, or an interface the type lists, without the general walk.
* `List<short>` and `List<sbyte>` indexers returned zero-extended values (-900 came back as 64636): the generic `ldelem.any` copied the
  element's bytes into a zeroed slot. `short` and `sbyte` now use the signed loads, as `ldelem.i2` and `ldelem.i1` do.
* `IntPtr.ToInt64()` (and any `(long)` of a pointer) could not be compiled on a 64-bit target: a 64-bit to 64-bit conversion chose an opcode that
  was "not used" on 32 bit and has no handler. It emits nothing now, as the 8 bytes are already the result.

## Tests

`tests/run_tests.py` runs, and requires `mcs`, `mono`, `ilasm` for the programs (every external run has a 60 s
timeout, `DNA_TEST_TIMEOUT`, so a hang fails its own check):

* the heap-tree differential test (original C vs the lowered C++, under ASan/UBSan);
* that the generated `MetaDataLayout.gen.h`, `JIT_Fused*.gen.h` and `Stencils.gen.h` are current, and that every native method reads its arguments at their
  real offsets for both pointer sizes (`tools/gen_metadata_layout.py --check`, `tools/check_internalcall_params.py`);
* a compile-time guard for the `JIT.c` uninitialised-variable bug;
* `tests/dotnet/*.cs`: C# programs, each compared with Mono (exit code, or whole output line for line);
  `ArithmeticMatrix` (91k cases), `CheckedConversions`, `UncheckedConversions`, `MathBits` are generated by
  `tests/gen_*.py`. `MathBits` compares every `Math`/`MathF` result bit for bit with real .NET (it needs a .NET SDK,
  and is skipped without one: Mono 6.8 lacks `Math.Log2`);
* `tests/il/*.il`: IL for opcodes C# does not emit, assembled once for Mono and once for DNA.
* `tests/dotnet/StencilRegisters.cs`, **generated from the table of register stencils** (`tools/gen_vstencil_tests.py`; the suite checks that
  it is current): a method for each shape of expression that one of the 851 stencils is for (operands that are locals, literals, 64-bit
  literals, a result in the register or a value in memory; results that go on in the register, to another local or back into the first
  operand; all six comparisons, and their negations for floating point, which are the unordered branches), run over awkward values (the
  extremes of each type, shift counts of 0 to 65, NaN, both zeros, infinities) and compared with Mono, plus whole-expression shapes for what the
  pass must get right between operations (two values alive at once, a store to a local that a pending operand describes, a value on the stack
  where two paths meet, a value held across a call). One program compiles all 851 stencils, and the suite checks that every stencil, plain and
  register (the plain ones through `DNA_NO_VSTACK=1`, which is also how they are reached for what the pass does not do), is compiled by some test.
  Not generated, because C# cannot produce them and so nothing could test them: a shift by a bare local (a compiler masks the count with `& 31`),
  `==` and `!=` with the constant first, `brtrue` on a computed 8-byte value, the conversion of a constant (folded by the compiler, and by the JIT). Those shapes
  take the plain stencils, which is also what the pass does for anything it does not know. 22 mutations of the pass and the templates (the wrong
  condition code, no unordered test, operands swapped, a pending operand not flushed before a store, nothing flushed at a branch target, two values in the
  register, the wrong pool constant, a popped operand not popped, ...) were all caught but two, which are equivalent: a value in the register that is
  duplicated is read correctly because anything that would overwrite the register spills every entry that names it, and an island is already preceded by a flush.

Files named `*.dnaonly.*` are self-checking programs for behaviour the reference runtime does not
share, `*.dnafail.il` must be refused by DNA with the stated message. Cases where Mono aborts or the
spec leaves the result unspecified (null `calli`, overlapping `cpblk`, an exception thrown from a filter)
are deliberately not compared.

`tests/crust_conformance.py` extracts the C# programs from Crust's `tests/test_cs*.py`, runs each on
Mono and on DNA, and requires the same exit code (or the same unhandled exception type). Programs
Crust itself refuses (passed to its `assert_refuses`) are excluded. Currently 55 of 55 usable programs agree, on both the 32-bit and the 64-bit build.

## Performance

DNA is an interpreter, and `tools/benchmark_mono.py` measures it against Mono's JIT on small programs (the same C# for both,
each timing its own work, with a checksum so that a wrong answer shows). Steady-state code is slower, by 1.1 to 40 times (most of it
by 2 to 9), and start-up, cold code, exceptions, string building, `Array.Copy` and calls into C (`[DllImport]`, see Native FFI below) are faster. What has been done about the first, each step
measured on its own and each switchable so that a suspected miscompile can be bisected:

* **The call path** (`MethodState_Direct`): one allocation for a frame, and inline copies for the few words of arguments,
  locals and return values. `recursion` and `alloc` are about 10% faster. There is no single hot spot left in it: the frame
  has too many fields to set up for much more.
* **Garbage collection scheduling** (`Heap.c`): a growing heap was collected every 200 KB however big it was, which is quadratic;
  the ceiling is now 64 MB (`-DMAX_HEAP_EXCESS`). Growing a 60,000-entry `Dictionary` went from 13 s to under a second, and
  `Dictionary` itself was rewritten as `buckets[]` + `entries[]`.
* **Fused instructions** (`tools/gen_fused_ops.py`, `FuseOps` in `JIT.c`): a run such as `ldloc a; ldloc b; add` becomes one
  instruction reading both locals, likewise local-op-constant, `i += k`, and compare-and-branch. It is never done across a
  branch target, a try block boundary or a debugger sequence point. `int_loop` 1.55x, `recursion`/`virtual_calls` 16-19%.
  `DNA_NO_FUSION=1` turns it (and native blocks) off.
* **Fast call and return** (`JIT_Execute.c`, `FAST_CALL`): `call` and `callvirt` (once the target is found in the vtable) make the new
  frame right in the handler instead of through `MethodState_Direct`, `CreateParameters` and a `Thread_StackAlloc` call: no test of
  which kind of call it is, only the locals cleared (the parameters are written by the copy of the arguments), the arguments copied
  with fixed-size moves for the usual sizes, and a returning frame is freed by resetting the thread-stack offset. Anything unusual (a
  method not yet compiled, a null `this`, a finalizer or delegate frame) goes the long way as before. 18% fewer instructions on
  `recursion`; in time `recursion` -22%, `list_int` and `struct_math` -14%, `virtual_calls` -8%. Off with `-DNO_FAST_CALL`, and
  automatically with `GEN_COMBINED_OPCODES`, `DIAG_METHOD_CALLS` or `_DEBUG`. Interface calls use it too (the interface map is searched
  in the handler), `ldelem.ref` reads the pointer directly, and a delegate with one target needs no copy of its arguments:
  `virtual_calls` is 21% faster than with none of it.
* **Garbage collection of candidate pointers** (`Heap.c`): marking looked up every word of every scanned object in the allocation tree,
  and worked out the size of each entry on the way down (`GetSize`: a type lookup, and a length for arrays and strings) just to
  choose left or right. A candidate outside the addresses of all heap entries, which is nearly every integer in an array of structs,
  is now dropped at once, and the search compares start addresses only and sizes the one entry it ends at (the end stays inclusive, as
  a zero-sized object needs). `dictionary` -44%, `list_int` -20%. `GcRoots` checks what must stay alive (references only inside arrays
  of structs, zero-sized objects, interior pointers held across collections, collections under allocation pressure, the evaluation
  stack, weak references) against Mono; it cannot see over-retention, which changes memory use and not output.
* **Native blocks** (`native/stencils/stencils.c`, `tools/gen_stencils.py`, `NativeBlocks.c`): copy-and-patch compilation, as in
  CPython's JIT. Each stencil is a tiny C function that does what one interpreter instruction does to the memory evaluation
  stack, compiled by gcc; the generator reads the object file, checks that it is straight-line code whose only relocations are
  holes, and writes the bytes and the holes into `Stencils.gen.h`. When the JIT finds a region made only of instructions that
  have stencils, it copies them into executable memory, patches the holes (a local's offset, a constant, a field offset, a
  branch target) and the region becomes one instruction. The stencils cover loads and stores of 4- and 8-byte locals,
  constants (a 64-bit one is two stencils), `float32`, `double`, 32-bit and 64-bit integer arithmetic, `int`/`long`/`float`/`double` conversions and the narrowing `conv.i1`..`conv.u4`, array access
  (`ldelem`/`stelem` of 4-byte elements and of bytes, `ldelema`, `ldlen`, with null and bounds checks that exit to the
  interpreter), `ldloca`/`ldfld`/`stfld`/`ldflda` on fields of 4 bytes and of 8 (`long`, `double`,
  references: structs in locals, `ref` arguments, objects), `dup`, and `br`/`brtrue`/`brfalse`, the six integer compare-and-branches and
  the ten `float32` and ten `double` ones (with NaN handled as the interpreter does) and the six `long` ones, so straight-line vector code, loops and conditionals are
  covered, as long as they contain nothing else.
  * **Calls are inlined.** A method whose whole body is one loop-free native block that calls and allocates nothing keeps a *recipe*
    (its stencils, holes and internal branch targets). A call to it from a block (`call`, or `callvirt` of a method that is not
    virtual) is replaced by: for `callvirt`, a check of `this`; the arguments, popped into an extension of the caller's frame;
    the callee's locals, cleared; then the callee's stencils with their frame offsets and branch targets moved. A leaf that
    inlined a leaf is still one recipe. A region that is the whole of a short method gets a recipe at any length, and a block
    containing an inlinable call counts it as worth a block. Not inlined: virtual targets (the object decides), recursion, anything
    that calls, allocates, boxes, loops, or has a handler. The callee is compiled when its caller is (so the cost of compiling it
    moves earlier, and a callee that is never called is still compiled). `DNA_NO_INLINE=1` turns it off, `DNA_INLINE_LIMIT=n`
    allows only the first n (to find by bisection which inline is at fault), and `DNA_FUSION_DEBUG=1` says for every call why it
    was or was not inlined and dumps each block. `vec_calls` 10.4 -> 3.3 ms and `vec_class` 5.7 -> 3.4 ms in A/B runs.
  * **Islands, and inlining methods with cold paths.** The evaluation stack is in memory, so the interpreter can run *any* instruction in the
    middle of a block: calls, `throw`, `newobj`, `ldstr`, casts, boxing and static fields are islands. The block leaves by an exit to a stub
    (the instruction's own words, then `JIT_NATIVE_RESUME block entry`, placed after the end of the method's ops) and is entered again at the
    stencil after it. Only in methods without exception handlers, because a stub is outside every `try` range. An island costs an exit, a stub
    and a re-entry, so a region needs 6 stencils of compiled code per island or it ends at its first island (without that rule `recursion` was
    38% slower). Islands let a *callee* with a cold path be inlined: its recipe carries its islands, `ldstr` becomes `JIT_LOAD_STRING_MD` with the
    metadata of the method it came from (a corlib `throw new ArgumentOutOfRangeException("index")` inlined into user code finds its string), and
    a branch to the method's own `ret` is a jump to the end of the recipe. `final` virtual methods (every interface implementation) and the
    methods of sealed classes are called directly. `List<int>`'s `Add`, `get_Count` and `get_Item` are now a few stencils in the caller's
    block (`list_int` 33.7 -> 8.7 ms). `DNA_NO_ISLANDS=1` turns islands off.
  * **Values in registers** (`native/src/VStack.c`, `tools/gen_vstencils.py`). The plain stencils each do what one instruction does to the
    evaluation stack in memory, so `x = a + b` is four of them and a round trip through memory for every value. A pass over the list
    of stencils keeps a *virtual* evaluation stack instead: a load of a local or a constant emits nothing and only records where the
    value is (a frame offset, an immediate, or the one cached register, `eax`/`rax`/`xmm0`); an operator takes its operands from where
    they are, and one three-address stencil does it and leaves the result in the register, or writes it to the local that the next
    instruction stores it to, or (`a += b`) updates the local in place. A first operand that was spilled to the stack in memory (there
    is only one register: `(a*b) ^ (c>>3)` spills one) is read from there and popped by the stencil; a 64-bit constant is read from a
    per-block pool (`HOLE5`, RIP-relative), and compares with a local or constant are a single `cmp`. There are 851 such stencils, one for each
    operator and each combination of where the operands are, written by `gen_vstencils.py` from templates. `i++; s += i; i < n` is 4 stencils
    instead of 12. Anything the pass does not do (a field, an array, a call, an island) takes its operands from the stack in memory, so
    the virtual stack is first written there; the same is done at every place that something can jump or be entered to, and at the end
    of the block, so the rest of the compiler never sees a value that is not in memory. A value that is only a description of a local is
    read late, so before a store to that local every entry that describes it is flushed (`a + (a = b)`). A recipe that is inlined stays in
    plain stencils and is optimised again in its caller's block. In one run: `vec_calls` 3.6x faster than without it, `vec_inline` 3.5x,
    `vec_struct` 2.5x, `double_loop` 2.3x, `vec_bounce` 2.3x, `sieve` 1.9x, `int_loop` 1.8x; what is bound by calls, allocation or memory
    (`alloc`, `boxing`, `dictionary`, `recursion`, `virtual_calls`) is within 5% either way. `DNA_NO_VSTACK=1` turns it off.
  * Same semantics as the interpreter: between the places where the pass keeps a value in a register the evaluation stack is in
    memory, and at every branch target, entry, island and exit it is, so the collector, exceptions and everything
    else see what they always did. A null reference in a field access makes the block return a status, and the interpreter
    throws `NullReferenceException` at that instruction, with the earlier effects done and the later ones not.
  * A block is entered only at its start (and, after giving up the processor, at the target of one of its backward
    branches); it is left through its exits, each of which continues at the target in the interpreter. A loop counts its
    backward branches against what is left of the thread's time slice (`numInst`, in `r14`), and when that runs out the
    block returns, the interpreter yields to another thread, and the block is entered again where the loop's back-edge goes.
    That entry is kept in the frame (`MethodState.nativeEntry`), because other threads run the same code.
  * x86-64 Linux only (the stencils use `r12`/`r13`/`r14`, and the blocks are in `mmap`ed executable memory, never freed);
    it is switched off elsewhere, if gcc or objdump is missing, and with `DNA_NO_STENCILS=1`. `DNA_BLOCK_MIN` (the shortest
    straight run that becomes a block, default 8), `DNA_NO_BLOCK_BRANCHES=1` (blocks without loops and conditionals),
    `DNA_STENCIL_STATS=1` (what was compiled) and `DNA_FUSION_DEBUG=1` (how each instruction was classified) are for finding
    out what it is doing; `DNA_OPCODE_TOP=n` lists more of a diagnostic build's opcode table.

Measured on one machine, best of 3 (`python3 tools/benchmark_mono.py`; Mono 6.8, DNA 64-bit release):

| | Mono ms | .NET 8 ms | DNA ms | DNA against Mono |
|---|---|---|---|---|
| `cold_methods` * | 6.4 | 15.7 | 0.03 | (see note) |
| `ffi_ref` | 28.8 | 5.3 | 1.5 | 19.8x faster |
| `ffi_sum6` | 110 | 4.9 | 10.0 | 10.9x faster |
| `ffi_add` | 110 | 4.4 | 10.3 | 10.7x faster |
| `ffi_mixed` | 114 | 5.8 | 11.8 | 9.63x faster |
| `string_concat` | 69.7 | 17.6 | 7.6 | 9.17x faster |
| `exceptions` | 9.0 | 29.0 | 1.0 | 8.84x faster |
| `ffi_buf` | 60.6 | 9.6 | 8.5 | 7.11x faster |
| `ffi_str` | 74.1 | 17.2 | 21.1 | 3.52x faster |
| `startup` | 9.2 | 26.3 | 3.2 | 2.92x faster |
| `vec_inline` | 3.9 | 1.1 | 1.7 | 2.26x faster |
| `ffi_echo` | 51.7 | 22.3 | 50.2 | about the same |
| `math_calls` | 15.3 | 3.3 | 15.0 | about the same |
| `array_copy` | 7.7 | 7.4 | 7.8 | about the same |
| `double_loop` | 3.3 | 2.0 | 3.4 | about the same |
| `vec_calls` | 1.1 | 0.82 | 1.3 | 1.2x slower |
| `vec_bounce` | 0.95 | 0.84 | 1.4 | 1.46x slower |
| `vec_struct` | 0.77 | 0.83 | 1.4 | 1.82x slower |
| `list_int` | 3.6 | 2.3 | 7.9 | 2.19x slower |
| `sieve` | 4.8 | 3.1 | 12.9 | 2.7x slower |
| `vec_array` | 0.16 | 1.1 | 0.56 | 3.48x slower |
| `vec_aos` | 0.20 | 1.0 | 0.85 | 4.26x slower |
| `int_loop` | 2.3 | 2.1 | 10.6 | 4.53x slower |
| `recursion` | 0.47 | 0.27 | 2.6 | 5.61x slower |
| `struct_math` | 6.2 | 0.55 | 37.2 | 5.96x slower |
| `vec_class` | 0.48 | 0.99 | 3.5 | 7.2x slower |
| `delegates` | 3.7 | 2.7 | 35.8 | 9.72x slower |
| `boxing` | 3.6 | 4.4 | 52.9 | 14.8x slower |
| `alloc` | 4.1 | 6.1 | 64.7 | 15.9x slower |
| `virtual_calls` | 2.7 | 3.4 | 50.6 | 18.8x slower |
| `dictionary` | 2.9 | 8.6 | 129 | 43.8x slower |

\* `cold_methods` times the first call of many methods. Inlining compiles a callee when its caller is compiled, which is before the
timer starts, so the timed region shrinks from about 1.7 ms to 0.04 ms; the whole run is not faster (3 ms against 2 ms with
`DNA_NO_INLINE=1`: compiling callees early costs a little). Read that row as an artifact, not a speedup. Absolute times also vary
by up to 2x between runs and sessions (the same build gave 10.5 ms for `int_loop` in one session and 19 ms in another while Mono and .NET 8 did not
move: code that goes through memory this much is sensitive to the host CPU); only ratios within one run, or interleaved A/B runs, mean anything. The
.NET 8 column is from the same run.

What that shows, and what it does not: the native blocks bring code that stays in locals, fields and arrays (`float32`, `int`,
`long`, `double`, references) between 2.3 times faster and 4.5 times slower than Mono's JIT (`vec_inline`, `vec_array`, `vec_bounce`, `vec_aos`, `vec_struct`,
`double_loop`, `sieve`, `int_loop`), where the plain interpreter is 15 to 25 times behind, and small methods that qualify, including ones with a cold path to throw or initialise, are part of
their callers' blocks (`vec_calls`, `list_int`). What is still interpreted and still 5 to 50 times behind is what calls something that is not a small
leaf (recursion, virtual and interface calls, delegates), allocates (`alloc`, `boxing`, `dictionary`: the collector and the allocation
tree), or uses what has no stencil yet: structs by value in
locals and arguments, `ldelem.ref`/`stelem.ref` and 2- and 8-byte array elements, `clt`/`ceq`. The register pass took the cost of the plain
stencils (about two cycles each, every value through memory) out of arithmetic and compares; what remains slower than .NET 8 there is mostly
what it does not yet do: field and array accesses still go through the stack in memory and so cost a flush, only one value is cached, and
there is no use of a value across a loop iteration. Against .NET 8 the code inside blocks is 0.5 to 5 times slower (`vec_array` and `vec_aos`
are faster, `int_loop` 5x and `sieve` 4x slower in the run below). The ordinary call path is about a quarter shorter than it was (it still builds a frame object of a dozen fields
per call), so what cannot be inlined costs somewhat less than it did, not a different order of magnitude. None of this has been tried on any other operating
system or CPU.

How a stencil is matched, and what that costs when it goes wrong. An instruction is recognised by the address of its handler, so
a stencil is only used for instructions the JIT emits as a distinct handler: `stelem.i1`/`.i2`/`.i4` are separate instructions
for that reason, and `ldelema` carries its element size. `ldelem.u1` and `stelem.i1` are used on `byte[]` (1-byte elements) and
on `bool[]` (4 bytes here), so the JIT picks the stride from the array's static type and falls back to the run-time
instruction when it does not know it. A match that is missed produces the right answer, only slower (`ldelem.r4` was missed for a
while, because its handler's address was not the one compared), so output comparisons cannot see it: look at what still runs
interpreted with a diagnostic build (`-DDIAG_OPCODE_USE`, `DNA_OPCODE_TOP=n`) or `DNA_FUSION_DEBUG=1`. The test suite checks that
every stencil is exercised by some stencil test, which catches a stencil nobody uses but not a missed source instruction, and a second
check fails if the classification compares some of the instructions that share a handler body but not all of them. Those do not share an
address: `JIT_LOAD_I64` and `JIT_LOAD_F64` are adjacent labels with different values (which is why `ldc.r8` was missed).

## Native FFI: `[DllImport]` of C functions (`build.py --ffi`)

On a native build (not the browser one) a `DllImport` could not call C at all: `PInvoke_GetFunction` returned the JavaScript bridge off Windows.
`build.py --ffi MANIFEST.json` builds the runtime with the C functions of a manifest in it (`build/dna_ffi`, with its own objects, so the normal
build is untouched), and calls to them are direct.

```json
{
  "c_files":  ["mylib.c"],
  "cflags":   ["-O2"],
  "functions": [
    {"library": "mylib", "entry": "add_numbers", "ret": "int", "args": ["int", "int"]}
  ]
}
```

```csharp
[DllImport("mylib", EntryPoint = "add_numbers", CallingConvention = CallingConvention.Cdecl)]
public static extern int AddNumbers(int a, int b);
```

Types (`tools/gen_ffi.py` has the full description; at most 6 arguments):

* `int uint short ushort sbyte byte long` (`int64_t`) `ulong` (`uint64_t`) `longlong ulonglong float double`, and `void` for a result.
* `intptr`: a pointer-sized value passed as it is: an `IntPtr`, a `ref` or `out` argument (the managed pointer itself: nothing moves in this
  runtime, so there is no pinning and no copy), an unsafe pointer. `"ref:int32_t*"` gives the C prototype's pointer type.
* `buf`: a C# array of a blittable type; C gets a pointer to its first element (null for null). Zero copy. `"buf:const double*"`.
* `cstr`: a C# `string`; C gets a temporary NUL-terminated UTF-8 copy for the duration of the call (on the stack if it fits in 256 bytes). As a result,
  a `char*` that becomes a string and is then freed with `free()`, which is what .NET does with a string result.

Not yet: structs by value (pass them by `ref`), callbacks, `StringBuilder`, more than 6 arguments. How it works (`tools/gen_ffi.py`, `native/src/FFI.c`):

* The generated unit declares each function from the manifest and then `#include`s your C files, so a definition that disagrees with the
  manifest is a compile error. (Name the functions without `static`, and avoid helper names that clash between files.)
* A `DllImport` is looked up in the generated table when the method that calls it is compiled (`FFI_Find`, by library and entry point; `libmylib.so`,
  `mylib.dll` and `mylib` are the same library). There is no `dlopen` and no name at run time. The C# declaration is checked against the
  manifest and a disagreement is refused when the method is compiled, with a message, instead of running with a corrupt stack.
* The call site is `JIT_FFI_CALL`: a generated wrapper reads the arguments where they lie on the evaluation stack, calls the function and
  writes the result where the first argument was. No frame, no copy, no marshalling. (A method reached any other way, such as through a
  delegate, calls the same wrapper from its frame.)
* **In a native block the call is stencils.** For each distinct signature the generator writes a stencil (assembly: each argument from its stack
  slot to its C ABI register, integers and floating point assigned independently; the stack aligned; a call through `r11`; a narrow result
  extended; the stack pointer adjusted) and two small ones put the function's address in `r11` from 32-bit holes. The loop around the call stays
  in one block. `gen_stencils.py --extra-c/--extra-names/--out` compiles them into a `Stencils.gen.h` of the FFI build's own.
* The functions run on the interpreter's thread, with the collector not involved; they must not call back into .NET. A signature with a string has
  no stencil (the wrapper converts it), and runs as an island in a block.

Measured against Mono 6.8 and against **.NET 8**, which is the stricter baseline: the same C#, the same C as a shared library (`benchmark_mono.py
--net8` builds all three; times are for the whole loop):

| | Mono 6.8 | .NET 8 | DNA | DNA against .NET 8 |
|---|---|---|---|---|
| `add_numbers(int, int)`, 2M calls | 109 ms | 4.7 ms | 7.3 ms | 1.6x slower |
| six integer arguments, 2M calls | 115 ms | 4.3 ms | 10.2 ms | 2.4x slower |
| int, double, long, float, int; double result, 2M | 119 ms | 5.1 ms | 11.2 ms | 2.2x slower |
| an `int[]` argument (C reads 16 elements), 500k | 66 ms | 9.5 ms | 8.2 ms | 1.2x **faster** |
| two `ref` arguments, 500k | 28 ms | 4.6 ms | 1.9 ms | 2.5x **faster** |
| a string argument, 500k | 72 ms | 14.1 ms | 22.7 ms | 1.6x slower |
| a string in and a string out, 200k | 52 ms | 21.0 ms | 47.3 ms | 2.3x slower |

DNA is 3 to 15 times faster than Mono and, against .NET 8, between 2.5x faster and 2.4x slower. The calls are not what costs: a loop of nothing but
`s += i` takes 2.9 ns an iteration here, as long as .NET 8 takes for a whole `DllImport` call, and the C call itself adds about 0.9 ns to it
(`add`: 3.8 ns). The rest is the code around the call, whose values still go through the evaluation stack in memory; keeping them in registers
is what would close the gap, for FFI and for everything else. The string results are slow because making a managed string is an allocation.

(Mono pays for a marshalling stub and a managed/native transition on every call.) `tests/dotnet/FfiCalls.cs` calls 14 functions (narrow results,
64-bit values, integer and floating-point arguments mixed, a pointer, state in C, delegates, a C function that needs an aligned stack) and
`FfiMarshal.cs` arrays, `ref`/`out` (into locals, array elements, struct and object fields, statics) and strings (empty, accents, CJK, a surrogate pair,
255/256 bytes and longer than the buffer, null); both are identical to Mono. The suite also checks that the generated stencils were compiled into blocks and
that a mismatched `DllImport` (an int for a long, an int for a string, a string for an int) is refused.
64-bit values, integer and floating-point arguments mixed, a pointer, state in C, delegates, a C function that needs an aligned stack) and is
identical to Mono; the suite also checks that the generated stencils were compiled into blocks and that a mismatched `DllImport` is refused.

## Where DNA deliberately differs

* `Marshal.SizeOf` accepts only unmanaged types (primitives, enums, structs of those) and refuses the
  rest; the reference accepts strings and arrays. Sizes use 64-bit natural alignment, `bool` is 1 byte.
* `Span<T>` is array-backed. `MemoryMarshal.CreateSpan`, `AsBytes` and `Cast` return **copies**, and
  `CreateSpan` supports only length 1.
* An exception thrown inside a filter is swallowed (the CLR rule; Mono differs).
* The C-library float functions (`Tan`, `Atan`, `Atan2`, `Sinh`, `Cosh`, `Tanh`, `Exp`, `Log10`, `Cbrt`, `Pow`) are
  the C library's, so they agree with .NET bit for bit on 64-bit x86-64 Linux and not on the 32-bit build (the i386
  `libm` returns different last bits). The 32-bit build is also compiled with SSE2 arithmetic (`-msse2 -mfpmath=sse`):
  the x87 default rounds `double` division twice and so differs from .NET.
* `jmp` runs as load-arguments, call, return (the stack grows); `tail.` and `volatile.` are ignored.

## 64-bit port (done for Linux x86-64)

DNA assumed 32-bit pointers in many independent places, so the port was staged, each stage gated by the whole
suite. A single source builds as 32-bit and 64-bit (wasm32 has not been tried); on a 32-bit target every change below
reduces to what it was before (for the layout changes, generated or compile-time checks pin that).

1. **Metadata loader.** Table rows were arrays of fixed 4-byte cells padded to the 32-bit struct size, with
   pointers stored through `(unsigned int)` casts. The loader is driven by `native/src/MetaDataLayout.gen.h`,
   generated by `tools/gen_metadata_layout.py` (which holds the file-format description and measures the structs
   with the compiler) using `offsetof()`. On a 32-bit target generated static asserts pin every offset and size
   to the old values. The generator also refuses a pointer-source column that lands in a non-pointer field (it
   caught `tMD_FieldRVA.rva`, a `U32` holding a pointer by coincidence). `run_tests.py` checks the file is current.
2. **Instruction stream.** The op stream was `U32` words, but an instruction's first word is the address of the
   code that runs it (`goto **(void**)pCurOp`) and many operands are pointers. The word type is `tOpWord`
   (`uintptr_t`); it covers the JIT's op arrays, the interpreter's `pOps`/`pCurOp`, and the opcode words of the
   `tJITCallNative` / `tJITCallPInvoke` structs that are overlaid on the stream. A 64-bit constant is one word on
   64-bit and two on 32-bit.
3. **References.** References, `this`, native ints and managed pointers are pointer-sized through the
   interpreter. The original aliased them to the 4-byte handlers ("only on 32-bit") or hard-coded 4: now
   `POP_O`, parameter/local/field/static loads and stores, `ldsflda`, `ldftn`, `box` of a native int,
   reference-array elements (`JIT_LOAD/STORE_ELEMENT_PTR`), `ldind.ref`/`stind.ref`, `brfalse`/`brtrue`
   (`JIT_BRANCH_*_PTR`), object and pointer comparisons, `stobj`, `conv.i`/`conv.u` (a native int is an int64 on
   64-bit) and the type table all have pointer-width versions. The collector scans every region at 4-byte steps,
   reading a pointer-sized value at each, so unaligned references are found.
4. **Natives.** Every native method reads its arguments from a block laid out by the stack-slot rules (4 bytes for
   an int/bool/char/float, 8 for a long/double, a pointer's width for a reference). Hand-written offsets such as
   `((U32*)pParams)[1]` assumed every earlier argument was 4 bytes. `tools/check_internalcall_params.py` compares
   each read with the native's registered signature for both pointer sizes (it found 46 wrong reads; `--fix`
   rewrote them as `PSZ`-based offsets) and runs as a test.
5. **Object layout.** Managed instance fields were packed one after another while the C structs they share
   (`Thread`, delegates, ...) pad each pointer to 8 bytes, so on 64-bit `Thread.currentCulture` overwrote the C
   `state` field and the scheduler spun. Fields are now naturally aligned (8 for a size that is a multiple of 8 on
   a 64-bit target, otherwise 4) and a type holding an 8-aligned field is rounded up to a multiple of 8.
   `Nullable<T>`'s `.Value` is read from its real field (a second operand on `box`/`unbox`), not from "+4".
6. **Two more places that depended on struct layout:** the fat exception-clause table was `memcpy`'d from the file
   over `tExceptionHeader` (24 bytes on 32-bit, 32 on 64-bit), so clauses are parsed field by field; and a catch or
   filter is entered with the exception reference on the evaluation stack, which no instruction pushes and so was
   never counted in the method's maximum stack (it fit on 32-bit only by luck, and on 64-bit overflowed into the
   parameters).

Results: `tests/run_tests.py` passes 38/38 on both `build/dna32` and `build/dna`, and the Crust survey passes
55/55 on both. On 64-bit `MathBits` matches .NET 8 on all 56 results; on 32-bit 15 results that call the C
library differ, because a 32-bit process calls the i386 `libm` (see "Where DNA deliberately differs").

Not done: x86-only P/Invoke has not been exercised on 64-bit (Prowl and the tests use internal calls); other
operating systems and architectures are untested; WebAssembly is paused (see above).

## Known gaps

* A class that implements two instantiations of the same generic interface (`IG<int>` and `IG<long>`) crashes on the first call
  through either. One instantiation per class works.
* `localloc` (`stackalloc`) and `__arglist` are not implemented. `stackalloc` needs a decision on
  raw-pointer access width, which this runtime currently infers from the address.
* 64-bit enums are handled as 32-bit values.
* `calli` takes only method pointers from `ldftn`/`ldvirtftn`; `TypedReference` has no `ToObject`/`MakeTypedReference`;
  filters nest at most 16 deep; `Enum.Parse` and `List<T>.Sort` do not exist.
* Float results of the C-library functions (`Tan`, `Atan`, `Exp`, `Pow`, ...) are those of the C library underneath:
  bit-identical to .NET on a 64-bit x86-64 Linux build, but not on the 32-bit build, and .NET itself is not
  bit-identical across architectures.
