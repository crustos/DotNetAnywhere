# DotNetAnywhere

DotNetAnywhere (DNA) is a small interpreted .NET CIL runtime written in C: it loads an assembly, JIT-translates each
method to its own compact instruction stream, runs it, and has a conservative garbage collector. The original
[code](https://github.com/chrisdunelm/DotNetAnywhere) was adjusted by Steve Sanderson in his first versions of
[Blazor](https://blazor.net). As he has now replaced DNA with Mono, I decided to fork his
[latest DNA version](https://github.com/SteveSanderson/Blazor/tree/150aeeb0965bd4b7a24412d239d836016c6b4238) in order
to adjust it for the needs of my demo project [The Wheel of WebAssembly](https://github.com/boyanio/wasm-wheel).

## This fork

This fork also builds **natively with gcc, as a 32-bit or a 64-bit runtime**, from the same source, so C# can run as a
scripting layer inside a native program. Compared with the version it started from:

* **32- and 64-bit.** The runtime assumed 32-bit pointers in many independent places. It now passes the same test
  suite as a 32-bit build (what WebAssembly uses) and as a native 64-bit build. See [NATIVE.md](NATIVE.md) for how.
* **Differential tests.** About 40 checks compare DNA with Mono, and with real .NET where Mono is not enough, instead
  of storing expected output: 91,000 arithmetic cases, every checked and unchecked conversion, exception unwinding,
  boxing and unboxing, `Math`/`MathF` bit for bit, hand-written IL for the opcodes C# never emits, and the C# programs
  from the [Crust](https://github.com/brentharts/crust) test suite.
* **Many runtime bugs fixed** (they were found by those tests): unwinding more than one frame, `finally` blocks,
  failed casts throwing the wrong object, integer division by zero killing the process, narrow array accesses through
  `ref`, the `Enum` text conversions, and others.
* **More of the language and library.** Exception filters (`catch ... when`) and `fault` blocks, `Marshal` /
  `MemoryMarshal` / `Span<T>` / `[StructLayout]`, `MathF` and the rest of `Math`, `Math.Clamp`, `calli`, `jmp`,
  `cpblk`/`initblk`, `__makeref`, and the opcodes that were missing.
* **The runtime is plain C.** (The heap-tracking tree was for a while a C++ module in the Crust subset, lowered to C during
  the build; it is C again, `native/src/HeapTree.c`, so the build needs nothing but `gcc` and `python3`.)

### Build and test

Everything is built by `build.py`; `make` just calls it. You need `gcc` and `python3`, and `mono-mcs`, `mono-runtime` and
`mono-devel` for the corlib and the tests (`gcc-multilib` only for the 32-bit build). A
[Crust](https://github.com/brentharts/crust) checkout next to this one is needed only for the conformance test.

```
make                                  # python3 build.py: build/dna (native, 64-bit on x86-64) and build/corlib.dll
make ARGS="--m32"                     # python3 build.py --m32: build/dna32 (32-bit)
make test                             # build, then the whole suite     (python3 tests/run_tests.py)
python3 build.py --ffi M.json            # the runtime with the C functions of a manifest built in (build/dna_ffi): [DllImport] of them is a direct call; see NATIVE.md
git clone https://github.com/brentharts/crust.git ../crust     # only for the next line
python3 tests/crust_conformance.py    # Crust's C# programs, DNA against Mono   (--32 / --64 choose the binary)
make clean
```

`native/src/MetaDataLayout.gen.h` is not in the repository: `build.py` generates it (with
`tools/gen_metadata_layout.py`, which needs no compiler) when it is missing or out of date.

Run a program with `build/dna prog.exe`. Compile it against the bundled class library:

```
mcs -nostdlib -r:build/corlib.dll prog.cs      # corlib.dll must sit beside prog.exe
```

(Roslyn works too: `csc -nostdlib -r:corlib.dll`, which is how modern C# can be compiled for DNA.)

### Status

| | 32-bit | 64-bit |
|---|---|---|
| `tests/run_tests.py` | 68 of 68 | 76 of 76 |
| Crust survey (`crust_conformance.py`) | 55 of 55 | 55 of 55 |
| `Math`/`MathF` against .NET 8 (`MathBits`) | 41 of 56 | 56 of 56 |

The 15 `Math`/`MathF` results that differ on the 32-bit build call the C library (`Tan`, `Exp`, `Pow`, ...), and a
32-bit process calls a different `libm` from the one .NET calls on x86-64. Every result that DNA computes itself
matches on both. Tested on Linux x86-64 with gcc only.

### Not done

* `stackalloc` (`localloc`) and `__arglist` are not implemented.
* Other operating systems and architectures are untested, and P/Invoke has not been exercised on a 64-bit Windows build.
* 64-bit enums are handled as 32-bit values; `Enum.Parse` and `List<T>.Sort` do not exist.

[NATIVE.md](NATIVE.md) has the details: what was added, every runtime bug fixed, how the 64-bit port was done, where
DNA deliberately differs from the reference runtimes, and the known gaps.

### Tools

| | |
|---|---|
| `build.py` | the one build script: gcc, cached and parallel, and generates the layout, fused-instruction and stencil headers; `--m32`, `--debug`, `--clean` |
| `Makefile` | `make` calls `python3 build.py`; `ARGS="..."` passes options, and `test` and `clean` forward too |
| `tools/gen_metadata_layout.py` | generates `native/src/MetaDataLayout.gen.h`, how each metadata table row maps onto its C struct, for any word size |
| `tools/gen_fused_ops.py` | generates the fused instructions (one instruction for a run such as `ldloc; ldloc; add`): opcode numbers, interpreter handlers, and what the JIT matches |
| `tools/gen_vstencils.py` | the 851 register (three-address) stencils, from templates: one for each operator and each place its operands can be (a local, a constant, the register, the stack in memory, a 64-bit constant) |
| `tools/gen_vstencil_tests.py` | `tests/dotnet/StencilRegisters.cs`, a method for each shape of expression that one of them is for, compared with Mono |
| `tools/gen_stencils.py` | turns `native/stencils/stencils.c` into machine-code templates with holes, for the native blocks (x86-64 Linux; needs gcc and objdump) |
| `tools/benchmark_mono.py` | 31 small C# benchmarks (7 of them `[DllImport]` calls, which build the `--ffi` runtime themselves), DNA against Mono and with `--net8` against .NET 8, with a table and a matplotlib chart (`--help`) |
| `tools/gen_ffi.py` | from a manifest of C files and functions (`build.py --ffi`): the wrappers, the table and the call stencils that make `[DllImport]` of them a direct call |
| `tools/check_internalcall_params.py` | checks every native method's argument reads against its registered signature, for 32- and 64-bit pointers (`--fix` rewrites them) |
| `tests/gen_*.py` | generate the large test programs |

## WebAssembly

`python3 build.py --wasm` builds with `clang --target=wasm32-wasi` (needs clang, lld, wasi-libc, llvm-ar) into `build/dna.wasm` plus a
`build/dna-wasm` launcher that runs it under node (`tools/run_wasm.mjs`, WASI). The same host also provides the wasm JIT import
`dna.emit_wasm`, so hot methods are compiled to wasm at run time (the module's indirect function table is exported and growable).
`python3 tests/run_tests.py --wasm` runs the suite on it.

* `--wasm --lib-only` (optionally with `--ffi manifest.json`) builds `build/libdna[_ffi]_wasm.a` to link into another wasm program; CC#'s
  `--wasm` does this. The x86-64 stencils are not used for wasm.
* `tools/run_wasm.mjs --app prog.wasm args...` runs a program that embeds DNA (argv[0] is the program's name, paths are not rewritten,
  `CCS_MANAGED_DLL` is set to `prog.managed.dll` when that exists, and a trap exits like abort(): SIGABRT).
* wasi-libc is musl, so a few `Math.*` results differ in the last bit from glibc/.NET; `MathBits` does not assert them for wasm.
