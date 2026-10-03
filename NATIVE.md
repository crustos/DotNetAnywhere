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
    python3 build.py --wasm               # WebAssembly: build/dna.js, build/dna.wasm (needs emcc)

`native/src/MetaDataLayout.gen.h` is generated, not committed: `build.py` runs `tools/gen_metadata_layout.py` when it
is missing or older than the files it is generated from, and `--clean` removes it. The generator works out the
32-bit layout itself from the struct declarations and the typedefs in `Types.h`, so it needs no compiler;
`--verify-with-compiler` (which the test suite passes when a 32-bit gcc exists) cross-checks it against `gcc -m32`.

Run a program with `build/dna prog.exe` (or `dna32`), compiled with `mcs -nostdlib -r:build/corlib.dll prog.cs` and
with `corlib.dll` beside the exe. `--debug` gives `-O0 -g`.

Both word sizes pass the same suite, and the tests do not depend on the pointer size: they compare with Mono (or
.NET), not with stored output. Tested on Linux x86-64 with gcc only.

**WebAssembly.** `build.py --wasm` carries over the flags of the old `native/build.sh` (all sources except
`NativeHost.c`, plus the lowered Crust module). It is only partly verified: every source compiles under Emscripten
3.1.6, which is the only version that was available, but that packaging links no `main` even for a four-line
program, so the link and the resulting `dna.js`/`dna.wasm` have never been run. `EXTRA_EXPORTED_RUNTIME_METHODS` is
deprecated in newer Emscripten (it warns) and the flags otherwise date from 1.38.

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

## Tests

`tests/run_tests.py` runs, and requires `mcs`, `mono`, `ilasm` for the programs (every external run has a 60 s
timeout, `DNA_TEST_TIMEOUT`, so a hang fails its own check):

* the heap-tree differential test (original C vs the lowered C++, under ASan/UBSan);
* that the generated `MetaDataLayout.gen.h` is current, and that every native method reads its arguments at their
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
suite. A single source builds as 32-bit (including wasm32) and 64-bit; on a 32-bit target every change below
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
operating systems and architectures are untested; the WebAssembly build has never been linked or run (see above).

## Known gaps

* `localloc` (`stackalloc`) and `__arglist` are not implemented. `stackalloc` needs a decision on
  raw-pointer access width, which this runtime currently infers from the address.
* 64-bit enums are handled as 32-bit values.
* `calli` takes only method pointers from `ldftn`/`ldvirtftn`; `TypedReference` has no `ToObject`/`MakeTypedReference`;
  filters nest at most 16 deep; `Enum.Parse` and `List<T>.Sort` do not exist.
* Float results of the C-library functions (`Tan`, `Atan`, `Exp`, `Pow`, ...) are those of the C library underneath:
  bit-identical to .NET on a 64-bit x86-64 Linux build, but not on the 32-bit build, and .NET itself is not
  bit-identical across architectures.
