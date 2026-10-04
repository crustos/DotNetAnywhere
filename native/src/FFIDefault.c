// The FFI table of a build with no manifest: no entries. A build with one (build.py --ffi) has the real table in FFI.gen.c, which overrides these
// (they are weak). They are here, apart from the code that reads them, because in the same file the compiler would see that the count is 0 and
// make everything that reads it a constant, whatever FFI.gen.c says.

#include "Compat.h"
#include "FFI.h"

__attribute__((weak)) const tFFIEntry ffiTable[1] = {{ NULL, NULL, NULL, NULL, 0, 0, NULL, 'v', NULL }};
__attribute__((weak)) const U32 ffiCount = 0;
