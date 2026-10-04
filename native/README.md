The DNA project is originally at https://github.com/chrisdunelm/DotNetAnywhere, though
has not recently been maintained (~ 5 years). See the 'src' directory for license info.

In this copy of the DNA code, various changes have been made, e.g.:
 - To support building with Emscripten
 - To support p/invoke calls from .NET to JavaScript
 - To add other interop primitives, e.g., GCHandle
 - To receive inbound calls from JavaScript to .NET
 - To fix some bugs
 - To support loading .NET Core-style assemblies

Likewise, the corlib.csproj project has been extended to support extra APIs.

HOW TO BUILD
============

Everything is built by `build.py` at the top of the repository (`make` calls it):

    python3 build.py              # the native runtime, build/dna, and build/corlib.dll
    python3 build.py --m32        # 32-bit native runtime, build/dna32

It needs gcc and a checkout of Crust (see ../README.md). The Emscripten build is gone (see "WebAssembly" in
../README.md); js-interop.js and the JSInterop/Debugger entry points are still here for a future wasm target, but
nothing builds or tests them now.
