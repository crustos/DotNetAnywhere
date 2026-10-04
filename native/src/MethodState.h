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

#if !defined(__METHODSTATE_H)
#define __METHODSTATE_H

typedef struct tMethodState_ tMethodState;

// Copy or clear a small block without calling into libc: arguments, locals and return values are a few words, and
// a call to memcpy/memset/memmove costs more than the copy. The blocks never overlap where these are used.
static inline void SmallCopy(void *dst, const void *src, U32 n) {
	unsigned char *d = (unsigned char*)dst;
	const unsigned char *s = (const unsigned char*)src;
	// the sizes that nearly every call and return has: a few fixed-size moves instead of a loop
	switch (n) {
	case 4: memcpy(d, s, 4); return;
	case 8: memcpy(d, s, 8); return;
	case 12: memcpy(d, s, 12); return;
	case 16: memcpy(d, s, 16); return;
	default: break;
	}
	if (n > 48) {
		memcpy(d, s, n);
		return;
	}
	for (; n >= 4; n -= 4, d += 4, s += 4) {
		*(U32*)d = *(const U32*)s;
	}
	for (; n > 0; n--) {
		*d++ = *s++;
	}
}

static inline void SmallZero(void *dst, U32 n) {
	unsigned char *d = (unsigned char*)dst;
	if (n > 48) {
		memset(d, 0, n);
		return;
	}
	for (; n >= 4; n -= 4, d += 4) {
		*(U32*)d = 0;
	}
	for (; n > 0; n--) {
		*d++ = 0;
	}
}


#include "MetaData.h"

struct tMethodState_ {
	// This method's meta-data
	tMetaData *pMetaData;
	// The method to execute
	tMD_MethodDef *pMethod;
	// The JITted code that this method-state is using.
	// When using the combined opcode JITter, this can vary between unoptimized and optimized.
	tJITted *pJIT;
	// The current offset into the method's JITted code (instruction offset, not byte offset)
	U32 ipOffset;
	// This method's evaluation stack
	PTR pEvalStack;
	// The evaluation stack current offset
	U32 stackOfs;
	// This method's parameters & local variable storage. Params are first, followed by locals
	PTR pParamsLocals;
	// Is this methodstate from a NEWOBJ op-code?
	U32 isInternalNewObjCall;
	// If this is a Finalizer, then the 'this' object goes here,
	// so it can be marked in the 'return' statement that it no longer has a Finalizer to run
	HEAP_PTR finalizerThis;
	// When in a delegate invoke, store the next delegate to invoke here.
	// This is to allow multi-cast delegates to call all their methods.
	void *pNextDelegate;
	// And store the parameters to go to this delegate call
	void *pDelegateParams;
	// When a leave instruction has to run a 'finally' bit of code, store the leave jump address here
	tOpWord *pOpEndFinally;
	// A leave may have to run several finally blocks (leaving nested try blocks), innermost first, so remember
	// where the leave was (an op offset) and the first clause that has not been looked at yet
	U32 leaveSrc;
	U32 leaveNext;
	// Where a native block that gave up the processor (its time slice ran out in a loop) is to be re-entered: the entry
	// number, 0 for its start. Per frame, because other threads may run the same code meanwhile.
	U32 nativeEntry;

#ifdef DIAG_METHOD_CALLS
	// For tracking execution time.
	U64 startTime;
#endif

	// Link to caller methodstate
	tMethodState *pCaller;

	// In the case of a reflection-initiated invocation (i.e., someMethodBase.Invoke(...)),
	// we need to track the target method's return type so we can interpret the stack when
	// it's done.
	tMD_TypeDef *pReflectionInvokeReturnType;
};

//void MethodState_Init();
tMethodState* MethodState_Direct(tThread *pThread, tMD_MethodDef *pMethod, tMethodState *pCaller, U32 isInternalNewObjCall);
tMethodState* MethodState(tThread *pThread, tMetaData *pMetaData, IDX_TABLE methodToken, tMethodState *pCaller);
void MethodState_Delete(tThread *pThread, tMethodState **ppMethodState);

#endif