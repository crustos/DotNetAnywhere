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

#if !defined(__SYS_H)
#define __SYS_H

#include "Config.h"

/*#ifdef _DEBUG
void* mallocTrace(int s, char *pFile, int line);
#define malloc(s) mallocTrace(s, __FILE__, __LINE__)
#endif*/

#ifdef _DEBUG
#define Assert(cond) if (!(cond)) Crash("Assert failed: %s, line %d", __FILE__, __LINE__);
#else
#define Assert(cond)
#endif

#define FAKE_RETURN exit(101)

// The size of a pointer. A native method's arguments are laid out one after another, each taking its
// evaluation-stack slot: 4 bytes for an int/bool/char/float, 8 for a long/double, PSZ for a reference or a
// pointer. So the argument after a reference and an int is at PSZ+4. (tools/check_internalcall_params.py
// checks every native's reads against its registered signature, for both pointer sizes.)
#define PSZ ((U32)sizeof(void*))
#define INTERNALCALL_PARAM(ofs, type) (*(type*)(pParams + (ofs)))

#include "MetaData.h"

// Prints a message and exits; never returns. Declared so the compiler knows that the
// `default: Crash(...)` arm of a switch leaves nothing uninitialised.
#if defined(__GNUC__) || defined(__clang__)
void Crash(char *pMsg, ...) __attribute__((noreturn));
#else
void Crash(char *pMsg, ...);
#endif

extern U32 logLevel;
void log_f(U32 level, char *pMsg, ...);

char* Sys_GetMethodDesc(tMD_MethodDef *pMethod);

void* mallocForever(U32 size);

U64 msTime();
#if defined(DIAG_METHOD_CALLS) || defined(DIAG_OPCODE_TIMES) || defined(DIAG_GC) || defined(DIAG_TOTAL_TIME)
U64 microTime();
#endif
void SleepMS(U32 ms);

#endif
