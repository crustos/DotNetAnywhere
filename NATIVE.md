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

Files named `*.dnaonly.*` are self-checking programs for behaviour the reference runtime does not
share, `*.dnafail.il` must be refused by DNA with the stated message. Cases where Mono aborts or the
spec leaves the result unspecified (null `calli`, overlapping `cpblk`, an exception thrown from a filter)
are deliberately not compared.

`tests/crust_conformance.py` extracts the C# programs from Crust's `tests/test_cs*.py`, runs each on
Mono and on DNA, and requires the same exit code (or the same unhandled exception type). Programs
Crust itself refuses (passed to its `assert_refuses`) are excluded. Currently 55 of 55 usable programs agree, on both the 32-bit and the 64-bit build.

## Performance

DNA is an interpreter, and `tools/benchmark_mono.py` measures it against Mono's JIT on small programs (the same C# for both,
each timing its own work, with a checksum so that a wrong answer shows). Steady-state code is slower, by 6 to 74 times, and
start-up, cold code, exceptions, string building and `Array.Copy` are faster. What has been done about the first, each step
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
* **Native blocks** (`native/stencils/stencils.c`, `tools/gen_stencils.py`, `NativeBlocks.c`): copy-and-patch compilation, as in
  CPython's JIT. Each stencil is a tiny C function that does what one interpreter instruction does to the memory evaluation
  stack, compiled by gcc; the generator reads the object file, checks that it is straight-line code whose only relocations are
  holes, and writes the bytes and the holes into `Stencils.gen.h`. When the JIT finds a region made only of instructions that
  have stencils, it copies them into executable memory, patches the holes (a local's offset, a constant, a field offset, a
  branch target) and the region becomes one instruction. The stencils cover loads and stores of 4- and 8-byte locals,
  constants (a 64-bit one is two stencils), `float32`, `double`, 32-bit and 64-bit integer arithmetic, `int`/`float` conversions and the narrowing `conv.i1`..`conv.u4`, array access
  (`ldelem`/`stelem` of 4-byte elements and of bytes, `ldelema`, `ldlen`, with null and bounds checks that exit to the
  interpreter), `ldloca`/`ldfld`/`stfld` on 4-byte fields
  (structs in locals, `ref` arguments, objects), `dup`, and `br`/`brtrue`/`brfalse`, the six integer compare-and-branches and
  the ten `float32` and ten `double` ones (with NaN handled as the interpreter does) and the six `long` ones, so straight-line vector code, loops and conditionals are
  covered, as long as they contain nothing else.
  * Same semantics as the interpreter: the evaluation stack is still in memory, so the collector, exceptions and everything
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

| | Mono ms | DNA ms | |
|---|---|---|---|
| `exceptions` | 10.8 | 1.4 | 7.64x faster |
| `string_concat` | 56.0 | 7.6 | 7.35x faster |
| `cold_methods` | 7.4 | 1.7 | 4.24x faster |
| `startup` | 8.0 | 2.4 | 3.34x faster |
| `array_copy` | 6.5 | 4.9 | 1.31x faster |
| `vec_inline` | 3.5 | 4.1 | 1.16x slower |
| `math_calls` | 14.8 | 22.3 | 1.51x slower |
| `double_loop` | 2.9 | 5.5 | 1.9x slower |
| `vec_array` | 0.13 | 0.42 | 3.14x slower |
| `vec_aos` | 0.17 | 0.59 | 3.39x slower |
| `vec_bounce` | 0.89 | 3.3 | 3.65x slower |
| `sieve` | 4.1 | 16.3 | 3.95x slower |
| `vec_struct` | 0.61 | 2.4 | 3.96x slower |
| `int_loop` | 1.9 | 11.1 | 5.9x slower |
| `recursion` | 0.38 | 3.4 | 8.95x slower |
| `vec_calls` | 0.85 | 9.2 | 10.8x slower |
| `list_int` | 3.6 | 41.7 | 11.6x slower |
| `vec_class` | 0.40 | 5.2 | 12.9x slower |
| `struct_math` | 4.8 | 71.3 | 14.9x slower |
| `boxing` | 3.0 | 47.2 | 15.7x slower |
| `virtual_calls` | 2.5 | 43.0 | 17x slower |
| `delegates` | 3.8 | 75.8 | 19.7x slower |
| `alloc` | 3.1 | 60.5 | 19.8x slower |
| `dictionary` | 2.7 | 226 | 84.5x slower |

What that shows, and what it does not: the native blocks bring code that stays in locals, fields and arrays (`float32`, `int`, `long`,
`double`) within 1.2 to 7 times of Mono's JIT (`vec_inline`, `vec_bounce`, `vec_array`, `vec_aos`, `vec_struct`, `double_loop`, `int_loop`),
where the plain interpreter is 15 to 25 times behind; anything that calls, allocates, boxes or does virtual dispatch is still interpreted
and still 10 to 80 times behind, and the calls dominate the particle benchmark (`vec_class`). Inside a block the cost is about two cycles a
stencil, because the evaluation stack is still in memory; keeping stack values in registers (a virtual stack, three-address stencils) is the
next step for that, and 2- and 8-byte array elements, `ldelem.ref`, value-producing comparisons (`clt`, `ceq`) and calls are what would
make more code eligible. None of this has been tried on any other operating system or CPU.

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

* `localloc` (`stackalloc`) and `__arglist` are not implemented. `stackalloc` needs a decision on
  raw-pointer access width, which this runtime currently infers from the address.
* 64-bit enums are handled as 32-bit values.
* `calli` takes only method pointers from `ldftn`/`ldvirtftn`; `TypedReference` has no `ToObject`/`MakeTypedReference`;
  filters nest at most 16 deep; `Enum.Parse` and `List<T>.Sort` do not exist.
* Float results of the C-library functions (`Tan`, `Atan`, `Exp`, `Pow`, ...) are those of the C library underneath:
  bit-identical to .NET on a 64-bit x86-64 Linux build, but not on the 32-bit build, and .NET itself is not
  bit-identical across architectures.
