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

#include "System.Math.h"

#include <math.h>

tAsyncCall* System_Math_Sin(PTR pThis_, PTR pParams, PTR pReturnValue) {
	*(double*)pReturnValue = sin(INTERNALCALL_PARAM(0, double));

	return NULL;
}

tAsyncCall* System_Math_Cos(PTR pThis_, PTR pParams, PTR pReturnValue) {
	*(double*)pReturnValue = cos(INTERNALCALL_PARAM(0, double));

	return NULL;
}

tAsyncCall* System_Math_Tan(PTR pThis_, PTR pParams, PTR pReturnValue) {
	*(double*)pReturnValue = tan(INTERNALCALL_PARAM(0, double));

	return NULL;
}

tAsyncCall* System_Math_Pow(PTR pThis_, PTR pParams, PTR pReturnValue) {
	*(double*)pReturnValue = pow(INTERNALCALL_PARAM(0, double), INTERNALCALL_PARAM(8, double));

	return NULL;
}

tAsyncCall* System_Math_Sqrt(PTR pThis_, PTR pParams, PTR pReturnValue) {
	*(double*)pReturnValue = sqrt(INTERNALCALL_PARAM(0, double));

	return NULL;
}

// ---- The rest of Math (double) and all of MathF (float). Each calls the C library function the
// reference runtime calls, so the results agree with it to the last bit: MathF.Sin is sinf, not
// (float)sin((double)x). Round is rint (round half to even, the default mode), as Math.Round is.
#define MATH_D1(Name, fn) tAsyncCall* System_Math_##Name(PTR pThis_, PTR pParams, PTR pReturnValue) { \
	*(double*)pReturnValue = fn(INTERNALCALL_PARAM(0, double)); return NULL; }
#define MATH_D2(Name, fn) tAsyncCall* System_Math_##Name(PTR pThis_, PTR pParams, PTR pReturnValue) { \
	*(double*)pReturnValue = fn(INTERNALCALL_PARAM(0, double), INTERNALCALL_PARAM(8, double)); return NULL; }
#define MATH_F1(Name, fn) tAsyncCall* System_MathF_##Name(PTR pThis_, PTR pParams, PTR pReturnValue) { \
	*(float*)pReturnValue = fn(INTERNALCALL_PARAM(0, float)); return NULL; }
#define MATH_F2(Name, fn) tAsyncCall* System_MathF_##Name(PTR pThis_, PTR pParams, PTR pReturnValue) { \
	*(float*)pReturnValue = fn(INTERNALCALL_PARAM(0, float), INTERNALCALL_PARAM(4, float)); return NULL; }

MATH_D1(Asin, asin)
MATH_D1(Acos, acos)
MATH_D1(Atan, atan)
MATH_D1(Sinh, sinh)
MATH_D1(Cosh, cosh)
MATH_D1(Tanh, tanh)
MATH_D1(Exp, exp)
MATH_D1(Log, log)
MATH_D1(Log10, log10)
MATH_D1(Log2, log2)
MATH_D1(Floor, floor)
MATH_D1(Ceiling, ceil)
MATH_D1(Round, rint)
MATH_D1(Truncate, trunc)
MATH_D1(Cbrt, cbrt)
MATH_D2(Atan2, atan2)
MATH_D2(IEEERemainder, remainder)

MATH_F1(Sin, sinf)
MATH_F1(Cos, cosf)
MATH_F1(Tan, tanf)
MATH_F1(Sqrt, sqrtf)
MATH_F1(Asin, asinf)
MATH_F1(Acos, acosf)
MATH_F1(Atan, atanf)
MATH_F1(Sinh, sinhf)
MATH_F1(Cosh, coshf)
MATH_F1(Tanh, tanhf)
MATH_F1(Exp, expf)
MATH_F1(Log, logf)
MATH_F1(Log10, log10f)
MATH_F1(Log2, log2f)
MATH_F1(Floor, floorf)
MATH_F1(Ceiling, ceilf)
MATH_F1(Round, rintf)
MATH_F1(Truncate, truncf)
MATH_F1(Cbrt, cbrtf)
MATH_F2(Pow, powf)
MATH_F2(Atan2, atan2f)
MATH_F2(IEEERemainder, remainderf)
