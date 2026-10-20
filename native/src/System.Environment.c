// Copyright (c) 2012 DotNetAnywhere
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

#include "Compat.h"
#include "Sys.h"

#include "System.Environment.h"

#include "Type.h"
#include "System.String.h"
#include "Heap.h"

#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

tAsyncCall* System_Environment_get_TickCount(PTR pThis_, PTR pParams, PTR pReturnValue) {
	I32 t;
	
	t = (I32)msTime();
	*(I32*)pReturnValue = t;

	return NULL;
}

tAsyncCall* System_Environment_GetOSVersionString(PTR pThis_, PTR pParams, PTR pReturnValue) {
	static HEAP_PTR versionString = NULL;
	if (versionString == NULL) {
		char ver[64];
#ifdef _WIN32
		OSVERSIONINFO osVer;
		osVer.dwOSVersionInfoSize = sizeof(OSVERSIONINFO);
		GetVersionEx(&osVer);
		sprintf(ver, "%d.%d.%d", osVer.dwMajorVersion, osVer.dwMinorVersion, osVer.dwBuildNumber);
#else
		strcpy(ver, "0.0.0");
#endif
		versionString = SystemString_FromCharPtrASCII(ver);
		Heap_MakeUndeletable(versionString);
	}
	*(HEAP_PTR*)pReturnValue = versionString;
	return NULL;
}

tAsyncCall* System_Environment_get_Platform(PTR pThis_, PTR pParams, PTR pReturnValue) {
#ifdef _WIN32
	OSVERSIONINFO osVer;
	osVer.dwOSVersionInfoSize = sizeof(OSVERSIONINFO);
	GetVersionEx(&osVer);
	*(U32*)pReturnValue = (osVer.dwPlatformId == VER_PLATFORM_WIN32_NT)?2:1; // _WIN32NT:_WIN32Windows
#else
	*(U32*)pReturnValue = 4; // UNIX
#endif
	return NULL;
}

// Environment.GetEnvironmentVariable(name): the variable's value, or null (ASCII: a value is read byte by byte)
tAsyncCall* System_Environment_GetEnvironmentVariable(PTR pThis_, PTR pParams, PTR pReturnValue) {
	HEAP_PTR name = ((HEAP_PTR*)pParams)[0];
	char buf[256];
	U32 len, i;
	STRING2 s;
	const char *v;
	*(HEAP_PTR*)pReturnValue = NULL;
	if (name == NULL) return NULL;
	s = SystemString_GetString(name, &len);
	if (len >= sizeof(buf)) return NULL;
	for (i = 0; i < len; i++) buf[i] = (char)s[i];
	buf[len] = 0;
	v = getenv(buf);
	if (v != NULL) *(HEAP_PTR*)pReturnValue = SystemString_FromCharPtrASCII((U8*)v);
	return NULL;
}

// Environment.SetEnvironmentVariable(name, value): a null or empty value removes the variable (ASCII, as GetEnvironmentVariable)
static char* Environment_CString(HEAP_PTR str) {
	U32 len, i;
	STRING2 s = SystemString_GetString(str, &len);
	char *c = (char*)malloc(len + 1);
	for (i = 0; i < len; i++) c[i] = (char)s[i];
	c[len] = 0;
	return c;
}

tAsyncCall* System_Environment_SetEnvironmentVariable(PTR pThis_, PTR pParams, PTR pReturnValue) {
	HEAP_PTR name = ((HEAP_PTR*)pParams)[0], value = ((HEAP_PTR*)pParams)[1];
	if (name == NULL) return NULL;
	char *n = Environment_CString(name);
	char *v = value == NULL ? NULL : Environment_CString(value);
#ifdef _WIN32
	char *kv = (char*)malloc(strlen(n) + (v ? strlen(v) : 0) + 2);
	sprintf(kv, "%s=%s", n, v ? v : "");
	_putenv(kv);
	free(kv);
#else
	if (v == NULL || v[0] == 0) unsetenv(n);
	else setenv(n, v, 1);
#endif
	free(n);
	free(v);
	return NULL;
}
