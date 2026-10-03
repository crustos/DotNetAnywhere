// Native (non-Emscripten) host stubs.
// Under Emscripten these symbols are provided by js-interop.js.
#ifndef __EMSCRIPTEN__

#include <stdio.h>
#include "Compat.h"
#include "Types.h"
#include "PInvoke.h"

// There is no JavaScript host on a native build. Calls routed through the
// JS bridge are logged and return NULL.
char* invokeJsFunc(STRING libName, STRING funcName, STRING arg0) {
	fprintf(stderr, "[native] invokeJsFunc(%s, %s) ignored: no JS host\n",
		libName ? (const char*)libName : "(null)", funcName ? (const char*)funcName : "(null)");
	return NULL;
}

#endif
