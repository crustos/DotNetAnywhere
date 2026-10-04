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
* **Part of the runtime is written in the Crust C++ subset** (the heap-tracking tree) and lowered to C by Crust's
  `cpprust` during the build.

### Build and test

Everything is built by `build.py`; `make` just calls it. You need `gcc`, `python3`, a
[Crust](https://github.com/brentharts/crust) checkout next to this one, and `mono-mcs`, `mono-runtime` and
`mono-devel` for the corlib and the tests (`gcc-multilib` only for the 32-bit build).

```
git clone https://github.com/brentharts/crust.git ../crust

make                                  # python3 build.py: build/dna (native, 64-bit on x86-64) and build/corlib.dll
make ARGS="--m32"                     # python3 build.py --m32: build/dna32 (32-bit)
make test                             # build, then the whole suite     (python3 tests/run_tests.py)
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
| `tests/run_tests.py` | 51 of 51 | 51 of 51 |
| Crust survey (`crust_conformance.py`) | 55 of 55 | 55 of 55 |
| `Math`/`MathF` against .NET 8 (`MathBits`) | 41 of 56 | 56 of 56 |

The 15 `Math`/`MathF` results that differ on the 32-bit build call the C library (`Tan`, `Exp`, `Pow`, ...), and a
32-bit process calls a different `libm` from the one .NET calls on x86-64. Every result that DNA computes itself
matches on both. Tested on Linux x86-64 with gcc only.

### Not done

* `stackalloc` (`localloc`) and `__arglist` are not implemented.
* The WebAssembly build (`python3 build.py --wasm`) is untested beyond compiling: all of the sources compile under
  Emscripten 3.1.6, but the only `emcc` available while this was written cannot link even a hello-world, so the
  link and the result have never been run. Other operating systems and architectures are untested, and P/Invoke
  has not been exercised on a 64-bit build.
* 64-bit enums are handled as 32-bit values; `Enum.Parse` and `List<T>.Sort` do not exist.

[NATIVE.md](NATIVE.md) has the details: what was added, every runtime bug fixed, how the 64-bit port was done, where
DNA deliberately differs from the reference runtimes, and the known gaps.

### Tools

| | |
|---|---|
| `build.py` | the one build script: gcc, cached and parallel, lowers the Crust module first and generates the layout, fused-instruction and stencil headers; `--m32`, `--debug`, `--clean` |
| `Makefile` | `make` calls `python3 build.py`; `ARGS="..."` passes options, and `test` and `clean` forward too |
| `tools/gen_metadata_layout.py` | generates `native/src/MetaDataLayout.gen.h`, how each metadata table row maps onto its C struct, for any word size |
| `tools/gen_fused_ops.py` | generates the fused instructions (one instruction for a run such as `ldloc; ldloc; add`): opcode numbers, interpreter handlers, and what the JIT matches |
| `tools/gen_stencils.py` | turns `native/stencils/stencils.c` into machine-code templates with holes, for the native blocks (x86-64 Linux; needs gcc and objdump) |
| `tools/benchmark_mono.py` | 22 small C# benchmarks, DNA against Mono, with a table and a matplotlib chart (`--help`) |
| `tools/check_internalcall_params.py` | checks every native method's argument reads against its registered signature, for 32- and 64-bit pointers (`--fix` rewrites them) |
| `tests/gen_*.py` | generate the large test programs |

## WebAssembly

Paused. The Emscripten build (`native/build.sh`, `build.cmd`, CMake, and for a while `build.py --wasm`) has been
removed. The plan is to target WebAssembly directly with `clang --target=wasm32`, and later to adapt the JIT to emit
wasm on the fly. The 32-bit build (`--m32`) is the closest thing to a wasm32 target until then; nothing here has been
built or run as WebAssembly with this fork's changes.
