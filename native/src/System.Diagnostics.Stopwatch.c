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

#include "System.Diagnostics.Stopwatch.h"

#ifndef _WIN32
#include <time.h>
#endif

// A monotonic clock, in nanoseconds (Stopwatch.Frequency is 1,000,000,000), so measuring an interval is not
// disturbed by the wall clock being set. The Windows branch has not been built or run.
tAsyncCall* System_Diagnostics_Stopwatch_GetTimestamp(PTR pThis_, PTR pParams, PTR pReturnValue) {
#ifdef _WIN32
	LARGE_INTEGER freq, count;
	QueryPerformanceFrequency(&freq);
	QueryPerformanceCounter(&count);
	// count * 1e9 / freq without overflowing 64 bits
	*(U64*)pReturnValue = (U64)(count.QuadPart / freq.QuadPart) * 1000000000ULL
		+ (U64)(count.QuadPart % freq.QuadPart) * 1000000000ULL / (U64)freq.QuadPart;
#else
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	*(U64*)pReturnValue = (U64)ts.tv_sec * 1000000000ULL + (U64)ts.tv_nsec;
#endif
	return NULL;
}
