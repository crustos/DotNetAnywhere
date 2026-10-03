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

You need `gcc` (plus `gcc-multilib` for the 32-bit build), `python3`, a [Crust](https://github.com/brentharts/crust)
checkout next to this one, and `mono-mcs`, `mono-runtime` and `mono-devel` for the corlib and the tests.

```
git clone https://github.com/brentharts/crust.git ../crust

python3 build.py --corlib             # build/dna (native, 64-bit on x86-64) and build/corlib.dll
python3 build.py --m32 --corlib       # build/dna32 (32-bit)
python3 tests/run_tests.py            # the whole suite   (--32 or --64 chooses the binary)
python3 tests/crust_conformance.py    # Crust's C# programs, DNA against Mono
```

Run a program with `build/dna prog.exe`. Compile it against the bundled class library:

```
mcs -nostdlib -r:build/corlib.dll prog.cs      # corlib.dll must sit beside prog.exe
```

(Roslyn works too: `csc -nostdlib -r:corlib.dll`, which is how modern C# can be compiled for DNA.)

### Status

| | 32-bit | 64-bit |
|---|---|---|
| `tests/run_tests.py` | 38 of 38 | 38 of 38 |
| Crust survey (`crust_conformance.py`) | 55 of 55 | 55 of 55 |
| `Math`/`MathF` against .NET 8 (`MathBits`) | 41 of 56 | 56 of 56 |

The 15 `Math`/`MathF` results that differ on the 32-bit build call the C library (`Tan`, `Exp`, `Pow`, ...), and a
32-bit process calls a different `libm` from the one .NET calls on x86-64. Every result that DNA computes itself
matches on both. Tested on Linux x86-64 with gcc only.

### Not done

* `stackalloc` (`localloc`) and `__arglist` are not implemented.
* The WebAssembly build (`native/build.sh`) was not run as part of this work, and other operating systems and
  architectures are untested. P/Invoke has not been exercised on a 64-bit build.
* 64-bit enums are handled as 32-bit values; `Enum.Parse` and `List<T>.Sort` do not exist.

[NATIVE.md](NATIVE.md) has the details: what was added, every runtime bug fixed, how the 64-bit port was done, where
DNA deliberately differs from the reference runtimes, and the known gaps.

### Tools

| | |
|---|---|
| `build.py` | the gcc build: cached, parallel, lowers the Crust module first; `--m32`, `--debug`, `--corlib` |
| `tools/gen_metadata_layout.py` | generates `native/src/MetaDataLayout.gen.h`, how each metadata table row maps onto its C struct, for any word size |
| `tools/check_internalcall_params.py` | checks every native method's argument reads against its registered signature, for 32- and 64-bit pointers (`--fix` rewrites them) |
| `tests/gen_*.py` | generate the large test programs |

## Build for WebAssembly

You would need Emscripten 1.38.6 (or above). You can build the WebAssembly interpreter by running the following:

```
cd native
build.cmd
```

Two files will be generated in the `build` folder:
- `dna.wasm`
- `dna.js`
