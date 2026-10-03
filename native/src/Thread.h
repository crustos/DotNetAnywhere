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

#if !defined(__THREAD_H)
#define __THREAD_H

typedef struct tThread_ tThread;
typedef struct tThreadStack_ tThreadStack;

#include "MetaData.h"
#include "MethodState.h"
#include "Heap.h"
#include "Types.h"

#define THREADSTACK_CHUNK_SIZE 10000

struct tThreadStack_ {
	// This chunk of stack memory
	unsigned char memory[THREADSTACK_CHUNK_SIZE];
	// Current offset into this memory chunk
	U32 ofs;
	// Pointer to the next chunk.
	tThreadStack *pNext;
};

// Exception filters (`catch ... when (...)`). Looking for a handler is a first pass that may have to
// stop and run filter code, in the frame that owns the filter, with the frames above it left
// exactly as they are (they still have finally blocks to run once a handler is chosen). One of these
// records a search that is waiting for a filter's verdict. A filter can itself call code that
// throws and catches, so they stack.
#define MAX_ACTIVE_FILTERS 16
typedef struct tFilterCtx_ tFilterCtx;
struct tFilterCtx_ {
	// the frame the exception was thrown in (the top of the frames the search is going through)
	tMethodState *pTopState;
	// the frame that owns the filter, and which clause of its method the filter belongs to
	tMethodState *pFrame;
	U32 clause;
	// that frame's instruction pointer and stack depth, which running the filter overwrites
	U32 savedIp;
	U32 savedStackOfs;
	// the exception being filtered
	HEAP_PTR savedException;
	// whether the search that was interrupted was itself for an exception thrown inside a filter
	U32 outerSearchInFilter;
};

struct tThread_ {
	// Stuff that's synced with Thread.cs
	// The threadID of this thread
	U32 threadID;
	// The delegate that this thread starts by executing
	PTR startDelegate;
	// The parameter to pass to the starting method (this is ignored if no parameter is needed).
	HEAP_PTR param;
	// The current state of the thread (running/paused/etc...)
	U32 state;
	// The current culture of the thread. Never accessed in C
	void *pCurrentCulture;

	// Stuff that is independant of Thread.cs
	// Note that the size of this can be anything we like, as the size of the Thread .NET type is ignored.

	// This thread's currently executing method
	tMethodState *pCurrentMethodState;
	// Thread exit value
	I32 threadExitValue;
	// The current exception object of this thread (for use by RETHROW)
	HEAP_PTR pCurrentExceptionObject;
	// When unwinding the stack after a throw, this keeps track of which finally clauses have already been executed
	U32 nextFinallyUnwindStack;
	// And the method state that we're aiming for..
	tMethodState *pCatchMethodState;
	// And the exception catch handler we're aiming for...
	tExceptionHeader *pCatchExceptionHandler;
	// Searching for a handler: the frame the exception was thrown in, the next frame to look at,
	// and the clause to resume at in it. Kept here, not in locals, so a search interrupted to run
	// a filter can be resumed from endfilter.
	tMethodState *pThrowTopState;
	tMethodState *pSearchFrame;
	U32 searchNextClause;
	// Is the exception being searched for one thrown from inside a running filter? Then nothing
	// outside that filter's own call chain may handle it: it is swallowed at the filter.
	U32 searchInFilter;
	// The filters currently running, innermost last. The array is only allocated when a thread first
	// runs a filter (almost none do), so that a thread that never does carries nothing for them.
	U32 numFilters;
	tFilterCtx *pFilterCtx;
	// If this thread is waiting on async data, then the details are stored here
	tAsyncCall *pAsync;
	// Does this thread start with a parameter?
	U32 hasParam;
	// Pointer to the first chunk of thread-stack memory
	tThreadStack *pThreadStack;

	// The next thread in the system (needed for garbage collection and theading)
	tThread *pNextThread;
};

// The thread has finished
#define THREAD_STATUS_EXIT 1
// The thread is still running, but has completed its timeslot
#define THREAD_STATUS_RUNNING 2
// The thread is waiting on some async data (sleep or IO)
#define THREAD_STATUS_ASYNC 3
// The thread has just exited a lock, so allow other threads to acquire it if they are waiting
#define THREAD_STATUS_LOCK_EXIT 4

#define ASYNC_LOCK_EXIT ((tAsyncCall*)0x00000001)

// These are the same as the C# definitions in corelib,
// and can be ORed together.
#define THREADSTATE_RUNNING			0x0000
#define THREADSTATE_BACKGROUND		0x0004
#define THREADSTATE_UNSTARTED		0x0008
#define THREADSTATE_STOPPED			0x0010
#define THREADSTATE_SUSPENDED		0x0040

extern int releaseBreakPoint;
extern int waitingOnBreakPoint;
extern int alwaysBreak;

U32 Internal_Debugger_Resume_Check(PTR pThis_, PTR pParams, PTR pReturnValue, tAsyncCall *pAsync);
tThread* Thread();
void Thread_SetEntryPoint(tThread *pThis, tMetaData *pMetaData, IDX_TABLE entryPointToken, PTR params, U32 paramBytes);
I32 Thread_Execute();
tThread* Thread_GetCurrent();
void* Thread_StackAlloc(tThread *pThread, U32 size);
void Thread_StackFree(tThread *pThread, void *pAddr);

void Thread_GetHeapRoots(tHeapRoots *pHeapRoots);

#endif
