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

#include "JIT.h"

#include "JIT_OpCodes.h"
#include "MetaData.h"
#include "MetaDataTables.h"
#include "Heap.h"
#include "Type.h"
#include "MethodState.h"
#include "NativeBlocks.h"
#include "Finalizer.h"
#include "Delegate.h"
#include "PInvoke.h"
#include "FFI.h"

#include "System.String.h"
#include "System.Array.h"
#include "System.Reflection.MethodBase.h"
#include "System.Diagnostics.Debugger.h"

#include <math.h>

// Global array which stores the absolute addresses of the start and end of all JIT code
// fragment machine code.
tJITCodeInfo jitCodeInfo[JIT_OPCODE_MAXNUM];
tJITCodeInfo jitCodeGoNext;

// Get the next op-code
#define GET_OP() *(pCurOp++)

// Push a PTR value on the top of the stack
#define PUSH_PTR(ptr) *(PTR*)pCurEvalStack = (PTR)(ptr); pCurEvalStack += sizeof(void*)
// Push an arbitrarily-sized value-type onto the top of the stack
#define PUSH_VALUETYPE(ptr, valueSize, stackInc) memcpy(pCurEvalStack, ptr, valueSize); pCurEvalStack += stackInc
// Push a U32 value on the top of the stack
#define PUSH_U32(value) *(U32*)pCurEvalStack = (U32)(value); pCurEvalStack += 4
// Push a U64 value on the top of the stack
#define PUSH_U64(value) *(U64*)pCurEvalStack = (U64)(value); pCurEvalStack += 8
// Push a float value on the top of the stack
#define PUSH_FLOAT(value) *(float*)pCurEvalStack = (float)(value); pCurEvalStack += 4;
// Push a double value on the top of the stack
#define PUSH_DOUBLE(value) *(double*)pCurEvalStack = (double)(value); pCurEvalStack += 8;
// Push a 4-byte heap pointer on to the top of the stack
#define PUSH_O(pHeap) *(void**)pCurEvalStack = (void*)(pHeap); pCurEvalStack += sizeof(void*)
// DUP4() duplicates the top 4 bytes on the eval stack
#define DUP4() *(U32*)pCurEvalStack = *(U32*)(pCurEvalStack - 4); pCurEvalStack += 4
// DUP8() duplicates the top 4 bytes on the eval stack
#define DUP8() *(U64*)pCurEvalStack = *(U64*)(pCurEvalStack - 8); pCurEvalStack += 8
// DUP() duplicates numBytes bytes from the top of the stack
#define DUP(numBytes) memcpy(pCurEvalStack, pCurEvalStack - numBytes, numBytes); pCurEvalStack += numBytes
// Pop a U32 value from the stack
#define POP_U32() (*(U32*)(pCurEvalStack -= 4))
// Pop a U64 value from the stack
#define POP_U64() (*(U64*)(pCurEvalStack -= 8))
// Pop a float value from the stack
#define POP_FLOAT() (*(float*)(pCurEvalStack -= 4))
// Pop a double value from the stack
#define POP_DOUBLE() (*(double*)(pCurEvalStack -= 8))
// Pop 2 U32's from the stack
#define POP_U32_U32(v1,v2) pCurEvalStack -= 8; v1 = *(U32*)pCurEvalStack; v2 = *(U32*)(pCurEvalStack + 4)
// Pop 2 U64's from the stack
#define POP_U64_U64(v1,v2) pCurEvalStack -= 16; v1 = *(U64*)pCurEvalStack; v2 = *(U64*)(pCurEvalStack + 8)
// Pop 2 F32's from the stack
#define POP_F32_F32(v1,v2) pCurEvalStack -= 8; v1 = *(float*)pCurEvalStack; v2 = *(float*)(pCurEvalStack + 4)
// Pop 2 F64's from the stack
#define POP_F64_F64(v1,v2) pCurEvalStack -= 16; v1 = *(double*)pCurEvalStack; v2 = *(double*)(pCurEvalStack + 8)
// Pop a PTR value from the stack
#define POP_PTR() (*(PTR*)(pCurEvalStack -= sizeof(void*)))
// Pop an arbitrarily-sized value-type from the stack (copies it to the specified memory location)
#define POP_VALUETYPE(ptr, valueSize, stackDec) memcpy(ptr, pCurEvalStack -= stackDec, valueSize)
// Pop a Object (heap) pointer value from the stack
#define POP_O() (*(HEAP_PTR*)(pCurEvalStack -= sizeof(void*)))
// POP() returns nothing - it just alters the stack offset correctly
#define POP(numBytes) pCurEvalStack -= numBytes
// POP_ALL() empties the evaluation stack
#define POP_ALL() pCurEvalStack = pCurrentMethodState->pEvalStack

#define STACK_ADDR(type) *(type*)(pCurEvalStack - sizeof(type))
// General binary ops
#define BINARY_OP(returnType, type1, type2, op) \
	pCurEvalStack -= sizeof(type1) + sizeof(type2) - sizeof(returnType); \
	*(returnType*)(pCurEvalStack - sizeof(returnType)) = \
	*(type1*)(pCurEvalStack - sizeof(returnType)) op \
	*(type2*)(pCurEvalStack - sizeof(returnType) + sizeof(type1))
// General unary ops
#define UNARY_OP(type, op) STACK_ADDR(type) = op STACK_ADDR(type)

// Set the new method state (for use when the method state changes - in calls mainly)
#define SAVE_METHOD_STATE() \
	pCurrentMethodState->stackOfs = (U32)(pCurEvalStack - pCurrentMethodState->pEvalStack); \
	pCurrentMethodState->ipOffset = (U32)(pCurOp - pOps)

#define LOAD_METHOD_STATE() \
	pCurrentMethodState = pThread->pCurrentMethodState; \
	pParamsLocals = pCurrentMethodState->pParamsLocals; \
	pCurEvalStack = pCurrentMethodState->pEvalStack + pCurrentMethodState->stackOfs; \
	pJIT = pCurrentMethodState->pJIT; \
	pOps = pJIT->pOps; \
	pOpSequencePoints = pJIT->pOpSequencePoints; \
	pCurOp = pOps + pCurrentMethodState->ipOffset

// The fast call and return (below) do what MethodState_Direct and MethodState_Delete do, for the usual frame. Those functions have
// more to do when GEN_COMBINED_OPCODES or DIAG_METHOD_CALLS is on (and _DEBUG makes the thread stack write guard words), so then the
// general path is used. -DNO_FAST_CALL turns it off.
#if !defined(GEN_COMBINED_OPCODES) && !defined(DIAG_METHOD_CALLS) && !defined(NO_FAST_CALL) && !defined(_DEBUG)
#define FAST_CALL_PATH 1
#else
#define FAST_CALL_PATH 0
#endif

#define CHANGE_METHOD_STATE(pNewMethodState) \
	SAVE_METHOD_STATE(); \
	pThread->pCurrentMethodState = pNewMethodState; \
	LOAD_METHOD_STATE()

// Easy access to method parameters and local variables
#define PARAMLOCAL_U32(offset) *(U32*)(pParamsLocals + offset)
#define PARAMLOCAL_U64(offset) *(U64*)(pParamsLocals + offset)

// Portable overflow-checked 64-bit arithmetic: each returns non-zero on overflow and always
// stores the wrapped result. (Checked against the compiler builtins in tests/.)
static int AddOvfI64(I64 a, I64 b, I64 *r) {
	U64 ur = (U64)a + (U64)b;
	*r = (I64)ur;
	return (int)((((U64)a ^ ur) & ((U64)b ^ ur)) >> 63);
}
static int SubOvfI64(I64 a, I64 b, I64 *r) {
	U64 ur = (U64)a - (U64)b;
	*r = (I64)ur;
	return (int)((((U64)a ^ (U64)b) & ((U64)a ^ ur)) >> 63);
}
static int MulOvfI64(I64 a, I64 b, I64 *r) {
	if (a == 0 || b == 0) {
		*r = 0;
		return 0;
	}
	*r = (I64)((U64)a * (U64)b);
	if ((a == -1 && b == (I64)0x8000000000000000ULL) || (b == -1 && a == (I64)0x8000000000000000ULL)) {
		return 1;
	}
	return *r / b != a;
}
static int AddOvfU64(U64 a, U64 b, U64 *r) { *r = a + b; return *r < a; }
static int SubOvfU64(U64 a, U64 b, U64 *r) { *r = a - b; return a < b; }
static int MulOvfU64(U64 a, U64 b, U64 *r) { *r = a * b; return a != 0 && *r / a != b; }

// Does the value on top of the evaluation stack fit the destination of a conv.ovf.*?
//   from: 0 int32, 1 uint32, 2 int64, 3 uint64, 4 float32, 5 float64
//   to:   0 I1, 1 U1, 2 I2, 3 U2, 4 I4, 5 U4, 6 I8, 7 U8   (ConvOvfTarget in JIT.c)
// Integers are compared by sign and magnitude; floats are truncated toward zero first, and a
// NaN fails both bounds, so it overflows.
static int ConvOvfFits(U32 from, U32 to, PTR pTop) {
	static const U64 negLimit[8] = {128, 0, 32768, 0, 2147483648ULL, 0, 0x8000000000000000ULL, 0};
	static const U64 posLimit[8] = {127, 255, 32767, 65535, 2147483647ULL, 4294967295ULL,
		0x7fffffffffffffffULL, 0xffffffffffffffffULL};
	static const double lo[8] = {-128.0, 0.0, -32768.0, 0.0, -2147483648.0, 0.0, -9223372036854775808.0, 0.0};
	static const double hiEx[8] = {128.0, 256.0, 32768.0, 65536.0, 2147483648.0, 4294967296.0,
		9223372036854775808.0, 18446744073709551616.0};

	if (from >= 4) {
		double d = (from == 4) ? (double)*(float*)(pTop - 4) : *(double*)(pTop - 8);
		d = trunc(d);
		return d >= lo[to] && d < hiEx[to];
	} else {
		int neg;
		U64 mag;
		switch (from) {
		case 0: {
			I32 v = *(I32*)(pTop - 4);
			neg = v < 0;
			mag = neg ? (U64)0 - (U64)(I64)v : (U64)v;
			break;
		}
		case 1:
			neg = 0;
			mag = *(U32*)(pTop - 4);
			break;
		case 2: {
			I64 v = *(I64*)(pTop - 8);
			neg = v < 0;
			mag = neg ? (U64)0 - (U64)v : (U64)v;
			break;
		}
		default:
			neg = 0;
			mag = *(U64*)(pTop - 8);
			break;
		}
		return mag <= (neg ? negLimit[to] : posLimit[to]);
	}
}

// The method that actually runs for a virtual or interface method called on an object of
// type pThisType (the same resolution callvirt does). NULL if the type does not implement it.
static tMD_MethodDef* ResolveVirtualMethod(tMD_MethodDef *pMethod, tMD_TypeDef *pThisType) {
	if (TYPE_ISINTERFACE(pMethod->pParentType)) {
		I32 i;
		// Searched backwards, so an interface implemented more than once in the hierarchy gets
		// its most recent implementation.
		for (i = (I32)pThisType->numInterfaces - 1; i >= 0; i--) {
			if (pThisType->pInterfaceMaps[i].pInterface == pMethod->pParentType) {
				if (pThisType->pInterfaceMaps[i].pVTableLookup != NULL) {
					return pThisType->pVTable[pThisType->pInterfaceMaps[i].pVTableLookup[pMethod->vTableOfs]];
				}
				return pThisType->pInterfaceMaps[i].ppMethodVLookup[pMethod->vTableOfs];
			}
		}
		return NULL;
	}
	if (METHOD_ISVIRTUAL(pMethod)) {
		return pThisType->pVTable[pMethod->vTableOfs];
	}
	return pMethod;
}

// Unboxing a value of type `held` as `expected`. The reference runtimes allow an exact match, or
// types with the same underlying type where an enum counts as its integer type: so int <-> an int
// enum, and one int enum <-> another, but not int <-> uint, int <-> long, or int <-> a byte enum.
static tMD_TypeDef* UnboxUnderlying(tMD_TypeDef *pType) {
	if (pType->pParent == types[TYPE_SYSTEM_ENUM]) {
		U32 i;
		for (i = 0; i < pType->numFields; i++) {
			if (!FIELD_ISSTATIC(pType->ppFields[i])) {
				return pType->ppFields[i]->pType;
			}
		}
	}
	return pType;
}
static int UnboxCompatible(tMD_TypeDef *pExpected, tMD_TypeDef *pHeld) {
	return pExpected == pHeld || UnboxUnderlying(pExpected) == UnboxUnderlying(pHeld);
}

// Indirect (ref/pointer) access of a 1- or 2-byte integer. Two things can be pointed at:
//  * a packed element of a byte[]/sbyte[]/short[]/ushort[]/char[]: really 1 or 2 bytes wide;
//  * anything else (locals, parameters, instance and static fields): a 4-byte slot holding
//    the value widened to 32 bits.
// Accessing a packed element as a whole word read its neighbours and, on a store, overwrote them.
static int IsPackedNarrowElement(void *p) {
	tMD_TypeDef *pType = Heap_GetObjectTypeContaining(p);
	return pType != NULL && TYPE_ISARRAY(pType) &&
		pType->pArrayElementType->arrayElementSize < pType->pArrayElementType->stackSize;
}

// cpobj: copy one value of type `pType` from *pSrc to *pDst. References copy as pointers; 4- and
// 8-byte values and structs byte for byte; 1- and 2-byte integers by element width where the
// address is a packed array element, and as a widened 4-byte slot otherwise (see above).
static void CopyObject(tMD_TypeDef *pType, PTR pDst, PTR pSrc) {
	if (!pType->isValueType) {
		*(void**)pDst = *(void**)pSrc;
	} else if (pType->arrayElementSize < pType->stackSize) {
		U32 size = pType->arrayElementSize, value;
		int isSigned = pType == types[TYPE_SYSTEM_SBYTE] || pType == types[TYPE_SYSTEM_INT16];
		if (IsPackedNarrowElement(pSrc)) {
			value = (size == 1) ? (isSigned ? (U32)(I32)*(I8*)pSrc : (U32)*(U8*)pSrc)
				: (isSigned ? (U32)(I32)*(I16*)pSrc : (U32)*(U16*)pSrc);
		} else {
			value = *(U32*)pSrc;
		}
		if (IsPackedNarrowElement(pDst)) {
			if (size == 1) { *(U8*)pDst = (U8)value; } else { *(U16*)pDst = (U16)value; }
		} else {
			*(U32*)pDst = value;
		}
	} else {
		memmove(pDst, pSrc, pType->arrayElementSize);
	}
}


// Throw a new exception of the given type. This must not assign `heapPtr` directly: several
// handlers declare a local of that name, which would receive the exception while the shared throw
// code reads the function-level one. (That is how every failed castclass used to throw some stale,
// unrelated object, or crash on a NULL one.) So the type goes through a function-level variable
// and a stub allocates it where only the function-level heapPtr is visible.
#define THROW(exType) throwEx = (exType); goto throwTyped
// For handlers that declare their own local `heapPtr` (which would shadow the one the shared
// throw code reads): jump to a stub that allocates into the function-level variable.
#define THROW_NULLREF() goto throwNullRef
// Array access: a null array is a NullReferenceException, an index outside 0..length-1 (a negative one is a huge unsigned
// number) an IndexOutOfRangeException. (None of the element handlers used to check: reading or writing past the end of an
// array touched whatever was there, and a null array crashed the process.)
#define CHECK_ARRAY(arr, index) \
	if ((arr) == NULL) { THROW_NULLREF(); } \
	if ((U32)(index) >= ((tSystemArray*)(arr))->length) { THROW(types[TYPE_SYSTEM_INDEXOUTOFRANGEEXCEPTION]); }

static void CheckIfCurrentInstructionHasBreakpoint(tMethodState* pMethodState, U32 opOffset, I32* pOpSequencePoints)
{
	if (pOpSequencePoints != NULL) {
		I32 currentOpSequencePoint = pOpSequencePoints[opOffset];
		if (currentOpSequencePoint >= 0) {
			CheckIfSequencePointIsBreakpoint(pMethodState, currentOpSequencePoint);
		}
	}
}

// Note: newObj is only set if a constructor is being called
static void CreateParameters(PTR pParamsLocals, tMD_MethodDef *pCallMethod, PTR *ppCurEvalStack, HEAP_PTR newObj) {
	U32 ofs;

	if (newObj != NULL) {
		// If this is being called from JIT_NEW_OBJECT then need to specially push the new object
		// onto parameter stack position 0
		*(HEAP_PTR*)pParamsLocals = newObj;
		ofs = sizeof(void*);   // 'this' is a pointer (this was a hard-coded 4)
	} else {
		ofs = 0;
	}
	*ppCurEvalStack -= pCallMethod->parameterStackSize - ofs;
	SmallCopy(pParamsLocals + ofs, *ppCurEvalStack, pCallMethod->parameterStackSize - ofs);
}

static tMethodState* RunFinalizer(tThread *pThread) {
	HEAP_PTR heapPtr = GetNextFinalizer();
	if (heapPtr != NULL) {
		// There is a pending finalizer, so create a MethodState for it and put it as next-to-run on the stack
		tMethodState *pFinalizerMethodState;
		tMD_TypeDef *pFinalizerType = Heap_GetType(heapPtr);

		pFinalizerMethodState = MethodState_Direct(pThread, pFinalizerType->pFinalizer, pThread->pCurrentMethodState, 0);
		// Mark this methodState as a Finalizer
		pFinalizerMethodState->finalizerThis = heapPtr;
		// Put the object on the stack (the object that is being Finalized)
		// Finalizers always have no parameters
		*(HEAP_PTR*)(pFinalizerMethodState->pParamsLocals) = heapPtr;
		//printf("--- FINALIZE ---\n");

		return pFinalizerMethodState;
	}
	return NULL;
}

#ifdef DIAG_OPCODE_TIMES
U64 opcodeTimes[JIT_OPCODE_MAXNUM];
static __inline unsigned __int64 __cdecl rdtsc() {
	__asm {
		rdtsc
	}
}
#endif

#ifdef DIAG_OPCODE_USE
U32 opcodeNumUses[JIT_OPCODE_MAXNUM];

#define OPCODE_USE(op) opcodeNumUses[op]++;

#else

#define OPCODE_USE(op)

#endif

#define CHECK_FOR_BREAKPOINT() \
	CheckIfCurrentInstructionHasBreakpoint(pCurrentMethodState, pCurOp - pOps, pOpSequencePoints);

#ifdef __GNUC__

#define GET_LABEL(var, label) var = &&label

#define GO_NEXT() \
	CHECK_FOR_BREAKPOINT(); \
	goto **(void**)(pCurOp++)

#else
#ifdef _WIN32

#define GET_LABEL(var, label) \
	{ __asm mov edi, label \
	__asm mov var, edi }

#define GO_NEXT() \
	CHECK_FOR_BREAKPOINT(); \
	{ __asm mov edi, pCurOp \
	__asm add edi, 4 \
	__asm mov pCurOp, edi \
	__asm jmp DWORD PTR [edi - 4] }

#endif
#endif

#define GO_NEXT_CHECK() \
	if (--numInst == 0) goto done; \
	GO_NEXT()

#define GET_LABELS(op) \
	GET_LABEL(pAddr, op##_start); \
	jitCodeInfo[op].pStart = pAddr; \
	GET_LABEL(pAddr, op##_end); \
	jitCodeInfo[op].pEnd = pAddr; \
	jitCodeInfo[op].isDynamic = 0

#define GET_LABELS_DYNAMIC(op, extraBytes) \
	GET_LABEL(pAddr, op##_start); \
	jitCodeInfo[op].pStart = pAddr; \
	GET_LABEL(pAddr, op##_end); \
	jitCodeInfo[op].pEnd = pAddr; \
	jitCodeInfo[op].isDynamic = 0x100 | (extraBytes & 0xff)

#define RUN_FINALIZER() {tMethodState *pMS = RunFinalizer(pThread);if(pMS) {CHANGE_METHOD_STATE(pMS);}}

// The next finally clause, from clause `from` on, that a `leave` at op `src` going to op `target` has to run: one whose
// try block contains the leave but not the target. Clauses are listed innermost first, which is the order they run in.
// (A leave used to run just the first one, so `return` from nested try/finally blocks skipped all the outer ones.)
static tExceptionHeader* FindLeaveFinally(tJITted *pJIT, U32 from, U32 src, U32 target, U32 *pNext) {
	U32 i;
	for (i = from; i < pJIT->numExceptionHandlers; i++) {
		tExceptionHeader *pEx = &pJIT->pExceptionHeaders[i];
		if (pEx->flags == COR_ILEXCEPTION_CLAUSE_FINALLY &&
			src >= pEx->tryStart && src < pEx->tryEnd &&
			!(target >= pEx->tryStart && target < pEx->tryEnd)) {
			*pNext = i + 1;
			return pEx;
		}
	}
	return NULL;
}

U32 JIT_Execute(tThread *pThread, U32 numInst) {
	tJITted *pJIT;
	tMethodState *pCurrentMethodState;
	PTR pParamsLocals;

	// Local copies of thread state variables, to speed up execution
	// Pointer to next op-code
	tOpWord *pOps;
	I32 *pOpSequencePoints;
	register tOpWord *pCurOp;
	// Pointer to eval-stack position
	register PTR pCurEvalStack;
	PTR pTempPtr;

	U32 op;
	// General purpose variables
	//I32 i32Value;
	U32 u32Value; //, u32Value2;
	//U64 u64Value;
	//double dValue;
	//float fValue;
	//uConvDouble convDouble;
	U32 ofs;
	tMD_TypeDef *throwEx;
	HEAP_PTR heapPtr;
	PTR pMem;

	if (pThread == NULL) {
		void *pAddr;
		// Special case to get all the label addresses
		// Default all op-codes to noCode.
		GET_LABEL(pAddr, noCode);
		for (u32Value = 0; u32Value < JIT_OPCODE_MAXNUM; u32Value++) {
			jitCodeInfo[u32Value].pStart = pAddr;
			jitCodeInfo[u32Value].pEnd = NULL;
			jitCodeInfo[u32Value].isDynamic = 0;
		}

		// Get GoNext code
		GET_LABEL(jitCodeGoNext.pStart, JIT_GoNext_start);
		GET_LABEL(jitCodeGoNext.pEnd, JIT_GoNext_end);
		jitCodeGoNext.isDynamic = 0;

		// Get all defined opcodes
		GET_LABELS_DYNAMIC(JIT_NOP, 0);
		GET_LABELS(JIT_RETURN);
		GET_LABELS_DYNAMIC(JIT_LOAD_I32, 4);
		GET_LABELS(JIT_BRANCH);
		GET_LABELS(JIT_LOAD_STRING);
		GET_LABELS(JIT_LOAD_STRING_MD);
		GET_LABELS(JIT_FFI_CALL);
		GET_LABELS(JIT_CALLVIRT_O);
		GET_LABELS(JIT_CALL_NATIVE);
		GET_LABELS(JIT_CALL_O);
		GET_LABELS(JIT_NEWOBJECT);
		GET_LABELS(JIT_LOAD_PARAMLOCAL_ADDR);
		GET_LABELS(JIT_CALL_PTR);
		GET_LABELS(JIT_BOX_CALLVIRT);
		GET_LABELS(JIT_INIT_VALUETYPE);
		GET_LABELS(JIT_NEW_VECTOR);
		GET_LABELS(JIT_NEWOBJECT_VALUETYPE);
		GET_LABELS(JIT_IS_INSTANCE);
		GET_LABELS(JIT_LOAD_NULL);
		GET_LABELS(JIT_UNBOX2VALUETYPE);
		GET_LABELS(JIT_UNBOX2OBJECT);
		GET_LABELS(JIT_LOAD_FIELD_ADDR);
		GET_LABELS(JIT_DUP_GENERAL);
		GET_LABELS_DYNAMIC(JIT_POP, 4);
		GET_LABELS(JIT_STORE_OBJECT_VALUETYPE);
		GET_LABELS(JIT_DEREF_CALLVIRT);
		GET_LABELS(JIT_STORE_ELEMENT);
		GET_LABELS(JIT_LEAVE);
		GET_LABELS(JIT_END_FINALLY);
		GET_LABELS(JIT_THROW);
		GET_LABELS(JIT_RETHROW);
		GET_LABELS(JIT_LOADOBJECT);
		GET_LABELS(JIT_LOAD_VECTOR_LEN);
		GET_LABELS(JIT_SWITCH);
		GET_LABELS(JIT_LOAD_ELEMENT_ADDR);
		GET_LABELS(JIT_CALL_INTERFACE);
		GET_LABELS(JIT_CALLI);
		GET_LABELS(JIT_CAST_CLASS);
		GET_LABELS(JIT_LOAD_ELEMENT);
		GET_LABELS(JIT_LOADFIELD_VALUETYPE);
		GET_LABELS(JIT_LOADFIELD);
		GET_LABELS(JIT_LOADFUNCTION);
		GET_LABELS(JIT_INVOKE_DELEGATE);
		GET_LABELS(JIT_INVOKE_SYSTEM_REFLECTION_METHODBASE);
		GET_LABELS(JIT_REFLECTION_DYNAMICALLY_BOX_RETURN_VALUE);
		GET_LABELS(JIT_CALL_PINVOKE);
		GET_LABELS_DYNAMIC(JIT_LOAD_I64, 8);
		GET_LABELS(JIT_INIT_OBJECT);
		GET_LABELS_DYNAMIC(JIT_DUP_4, 0);
		GET_LABELS_DYNAMIC(JIT_DUP_8, 0);
		GET_LABELS(JIT_LOADSTATICFIELDADDRESS_CHECKTYPEINIT);
		GET_LABELS_DYNAMIC(JIT_POP_4, 0);
		GET_LABELS_DYNAMIC(JIT_LOAD_F32, 4);

		GET_LABELS_DYNAMIC(JIT_LOADPARAMLOCAL_INT64, 4);
		GET_LABELS_DYNAMIC(JIT_LOADPARAMLOCAL_INT32, 4);
		GET_LABELS_DYNAMIC(JIT_LOADPARAMLOCAL_INTNATIVE, 4);
		GET_LABELS_DYNAMIC(JIT_LOADPARAMLOCAL_F32, 4);
		GET_LABELS_DYNAMIC(JIT_LOADPARAMLOCAL_F64, 4);
		GET_LABELS_DYNAMIC(JIT_LOADPARAMLOCAL_PTR, 4);
		GET_LABELS_DYNAMIC(JIT_LOADPARAMLOCAL_O, 4);
		GET_LABELS(JIT_LOADPARAMLOCAL_VALUETYPE);

		GET_LABELS_DYNAMIC(JIT_LOADPARAMLOCAL_0, 0);
		GET_LABELS_DYNAMIC(JIT_LOADPARAMLOCAL_1, 0);
		GET_LABELS_DYNAMIC(JIT_LOADPARAMLOCAL_2, 0);
		GET_LABELS_DYNAMIC(JIT_LOADPARAMLOCAL_3, 0);
		GET_LABELS_DYNAMIC(JIT_LOADPARAMLOCAL_4, 0);
		GET_LABELS_DYNAMIC(JIT_LOADPARAMLOCAL_5, 0);
		GET_LABELS_DYNAMIC(JIT_LOADPARAMLOCAL_6, 0);
		GET_LABELS_DYNAMIC(JIT_LOADPARAMLOCAL_7, 0);

		GET_LABELS_DYNAMIC(JIT_STOREPARAMLOCAL_INT64, 4);
		GET_LABELS_DYNAMIC(JIT_STOREPARAMLOCAL_INT32, 4);
		GET_LABELS_DYNAMIC(JIT_STOREPARAMLOCAL_INTNATIVE, 4);
		GET_LABELS_DYNAMIC(JIT_STOREPARAMLOCAL_F32, 4);
		GET_LABELS_DYNAMIC(JIT_STOREPARAMLOCAL_F64, 4);
		GET_LABELS_DYNAMIC(JIT_STOREPARAMLOCAL_PTR, 4);
		GET_LABELS_DYNAMIC(JIT_STOREPARAMLOCAL_O, 4);
		GET_LABELS(JIT_STOREPARAMLOCAL_VALUETYPE);

		GET_LABELS_DYNAMIC(JIT_STOREPARAMLOCAL_0, 0);
		GET_LABELS_DYNAMIC(JIT_STOREPARAMLOCAL_1, 0);
		GET_LABELS_DYNAMIC(JIT_STOREPARAMLOCAL_2, 0);
		GET_LABELS_DYNAMIC(JIT_STOREPARAMLOCAL_3, 0);
		GET_LABELS_DYNAMIC(JIT_STOREPARAMLOCAL_4, 0);
		GET_LABELS_DYNAMIC(JIT_STOREPARAMLOCAL_5, 0);
		GET_LABELS_DYNAMIC(JIT_STOREPARAMLOCAL_6, 0);
		GET_LABELS_DYNAMIC(JIT_STOREPARAMLOCAL_7, 0);

		GET_LABELS(JIT_STOREFIELD_INT64);
		GET_LABELS(JIT_STOREFIELD_INT32);
		GET_LABELS(JIT_STOREFIELD_INTNATIVE);
		GET_LABELS(JIT_STOREFIELD_F32);
		GET_LABELS(JIT_STOREFIELD_F64);
		GET_LABELS(JIT_STOREFIELD_PTR);
		GET_LABELS(JIT_STOREFIELD_O);
		GET_LABELS(JIT_STOREFIELD_VALUETYPE);

		GET_LABELS(JIT_LOADSTATICFIELD_CHECKTYPEINIT_INT32);
		GET_LABELS(JIT_LOADSTATICFIELD_CHECKTYPEINIT_VALUETYPE);
		GET_LABELS(JIT_LOADSTATICFIELD_CHECKTYPEINIT_O);
		GET_LABELS(JIT_LOADSTATICFIELD_CHECKTYPEINIT_INTNATIVE);
		GET_LABELS(JIT_LOADSTATICFIELD_CHECKTYPEINIT_PTR);
		GET_LABELS(JIT_LOADSTATICFIELD_CHECKTYPEINIT_F32);
		GET_LABELS(JIT_LOADSTATICFIELD_CHECKTYPEINIT_F64);
		GET_LABELS(JIT_LOADSTATICFIELD_CHECKTYPEINIT_INT64);

		GET_LABELS(JIT_STORESTATICFIELD_INT32);
		GET_LABELS(JIT_STORESTATICFIELD_INT64);
		GET_LABELS(JIT_STORESTATICFIELD_O);
		GET_LABELS(JIT_STORESTATICFIELD_F32);
		GET_LABELS(JIT_STORESTATICFIELD_F64);
		GET_LABELS(JIT_STORESTATICFIELD_INTNATIVE);
		GET_LABELS(JIT_STORESTATICFIELD_PTR);
		GET_LABELS(JIT_STORESTATICFIELD_VALUETYPE);

		GET_LABELS(JIT_BOX_INT64);
		GET_LABELS(JIT_BOX_INT32);
		GET_LABELS(JIT_BOX_INTNATIVE);
		GET_LABELS(JIT_BOX_PTR);
		GET_LABELS(JIT_BOX_F32);
		GET_LABELS(JIT_BOX_F64);
		GET_LABELS(JIT_BOX_O);
		GET_LABELS(JIT_BOX_VALUETYPE);

		GET_LABELS_DYNAMIC(JIT_CEQ_I32I32, 0);
		GET_LABELS_DYNAMIC(JIT_CGT_I32I32, 0);
		GET_LABELS_DYNAMIC(JIT_CGT_UN_I32I32, 0);
		GET_LABELS_DYNAMIC(JIT_CLT_I32I32, 0);
		GET_LABELS_DYNAMIC(JIT_CLT_UN_I32I32, 0);
		GET_LABELS_DYNAMIC(JIT_CEQ_I64I64, 0);
		GET_LABELS_DYNAMIC(JIT_CGT_I64I64, 0);
		GET_LABELS_DYNAMIC(JIT_CGT_UN_I64I64, 0);
		GET_LABELS_DYNAMIC(JIT_CLT_I64I64, 0);
		GET_LABELS_DYNAMIC(JIT_CLT_UN_I64I64, 0);

		GET_LABELS_DYNAMIC(JIT_ADD_OVF_I32I32, 0);
		GET_LABELS_DYNAMIC(JIT_ADD_OVF_UN_I32I32, 0);
		GET_LABELS_DYNAMIC(JIT_MUL_OVF_I32I32, 0);
		GET_LABELS_DYNAMIC(JIT_MUL_OVF_UN_I32I32, 0);
		GET_LABELS_DYNAMIC(JIT_SUB_OVF_I32I32, 0);
		GET_LABELS_DYNAMIC(JIT_SUB_OVF_UN_I32I32, 0);
		GET_LABELS_DYNAMIC(JIT_ADD_I32I32, 0);
		GET_LABELS_DYNAMIC(JIT_SUB_I32I32, 0);
		GET_LABELS_DYNAMIC(JIT_MUL_I32I32, 0);
		GET_LABELS_DYNAMIC(JIT_DIV_I32I32, 0);
		GET_LABELS_DYNAMIC(JIT_DIV_UN_I32I32, 0);
		GET_LABELS_DYNAMIC(JIT_REM_I32I32, 0);
		GET_LABELS_DYNAMIC(JIT_REM_UN_I32I32, 0);
		GET_LABELS_DYNAMIC(JIT_AND_I32I32, 0);
		GET_LABELS_DYNAMIC(JIT_OR_I32I32, 0);
		GET_LABELS_DYNAMIC(JIT_XOR_I32I32, 0);
		GET_LABELS_DYNAMIC(JIT_NEG_I32, 0);
		GET_LABELS_DYNAMIC(JIT_NOT_I32, 0);
		GET_LABELS_DYNAMIC(JIT_NEG_I64, 0);
		GET_LABELS_DYNAMIC(JIT_NEG_F32, 0);
		GET_LABELS_DYNAMIC(JIT_NEG_F64, 0);
		GET_LABELS_DYNAMIC(JIT_COPYOBJECT, 4);
		GET_LABELS_DYNAMIC(JIT_LOADVIRTFUNCTION, 4);
		GET_LABELS_DYNAMIC(JIT_MKREFANY, 4);
		GET_LABELS_DYNAMIC(JIT_REFANYVAL, 4);
		GET_LABELS_DYNAMIC(JIT_REFANYTYPE, 0);
		GET_LABELS_DYNAMIC(JIT_JMP_COPYARGS, 0);
		GET_LABELS_DYNAMIC(JIT_CPBLK, 0);
		GET_LABELS_DYNAMIC(JIT_UNBOX, 4);
		GET_LABELS(JIT_ENDFILTER);
		GET_LABELS_DYNAMIC(JIT_INITBLK, 0);
		GET_LABELS_DYNAMIC(JIT_CKFINITE_F32, 0);
		GET_LABELS_DYNAMIC(JIT_CKFINITE_F64, 0);
		GET_LABELS_DYNAMIC(JIT_NOT_I64, 0);

		GET_LABELS(JIT_BOX_NULLABLE);
		GET_LABELS_DYNAMIC(JIT_LOAD_F64, 8);
		GET_LABELS(JIT_UNBOX_NULLABLE);

		GET_LABELS(JIT_BEQ_I32I32);
		GET_LABELS(JIT_BEQ_I64I64);
		GET_LABELS(JIT_BEQ_F32F32);
		GET_LABELS(JIT_BEQ_F64F64);

		GET_LABELS(JIT_BGE_I32I32);
		GET_LABELS(JIT_BGE_I64I64);
		GET_LABELS(JIT_BGE_F32F32);
		GET_LABELS(JIT_BGE_F64F64);
		GET_LABELS(JIT_BGE_UN_F32F32);
		GET_LABELS(JIT_BGE_UN_F64F64);

		GET_LABELS(JIT_BGT_I32I32);
		GET_LABELS(JIT_BGT_I64I64);
		GET_LABELS(JIT_BGT_F32F32);
		GET_LABELS(JIT_BGT_F64F64);
		GET_LABELS(JIT_BGT_UN_F32F32);
		GET_LABELS(JIT_BGT_UN_F64F64);

		GET_LABELS(JIT_BLE_I32I32);
		GET_LABELS(JIT_BLE_I64I64);
		GET_LABELS(JIT_BLE_F32F32);
		GET_LABELS(JIT_BLE_F64F64);
		GET_LABELS(JIT_BLE_UN_F32F32);
		GET_LABELS(JIT_BLE_UN_F64F64);

		GET_LABELS(JIT_BLT_I32I32);
		GET_LABELS(JIT_BLT_I64I64);
		GET_LABELS(JIT_BLT_F32F32);
		GET_LABELS(JIT_BLT_F64F64);
		GET_LABELS(JIT_BLT_UN_F32F32);
		GET_LABELS(JIT_BLT_UN_F64F64);

		GET_LABELS(JIT_BNE_UN_I32I32);
		GET_LABELS(JIT_BNE_UN_I64I64);
		GET_LABELS(JIT_BNE_UN_F32F32);
		GET_LABELS(JIT_BNE_UN_F64F64);

		GET_LABELS(JIT_BGE_UN_I32I32);
		GET_LABELS(JIT_BGT_UN_I32I32);
		GET_LABELS(JIT_BLE_UN_I32I32);
		GET_LABELS(JIT_BLT_UN_I32I32);
		GET_LABELS(JIT_BGE_UN_I64I64);
		GET_LABELS(JIT_BGT_UN_I64I64);
		GET_LABELS(JIT_BLE_UN_I64I64);
		GET_LABELS(JIT_BLT_UN_I64I64);

		GET_LABELS_DYNAMIC(JIT_SHL_I32, 0);
		GET_LABELS_DYNAMIC(JIT_SHR_I32, 0);
		GET_LABELS_DYNAMIC(JIT_SHR_UN_I32, 0);
		GET_LABELS_DYNAMIC(JIT_SHL_I64, 0);
		GET_LABELS_DYNAMIC(JIT_SHR_I64, 0);
		GET_LABELS_DYNAMIC(JIT_SHR_UN_I64, 0);

		GET_LABELS(JIT_BRANCH_FALSE);
		GET_LABELS(JIT_BRANCH_FALSE_PTR);
		GET_LABELS(JIT_BRANCH_TRUE_PTR);
#define FUSED_LABELS
#include "JIT_Fused.gen.h"
#undef FUSED_LABELS
		GET_LABELS(JIT_NATIVE_BLOCK);
		GET_LABELS(JIT_NATIVE_LOOP);
		GET_LABELS(JIT_NATIVE_RESUME);
		GET_LABELS(JIT_STORE_ELEMENT_I1);
		GET_LABELS(JIT_STORE_ELEMENT_I2);
		GET_LABELS(JIT_STORE_ELEMENT_I4);
		GET_LABELS(JIT_LOAD_ELEMENT_ADDR_N);
		GET_LABELS(JIT_LOAD_ELEMENT_U8_1);
		GET_LABELS(JIT_LOAD_ELEMENT_U8_4);
		GET_LABELS(JIT_BRANCH_TRUE);
		GET_LABELS(JIT_LOADTOKEN_TYPE);
		
		GET_LABELS(JIT_LOADTOKEN_FIELD);
		GET_LABELS(JIT_LOADINDIRECT_I8);
		GET_LABELS(JIT_LOADINDIRECT_U8);
		GET_LABELS(JIT_LOADINDIRECT_I16);
		GET_LABELS(JIT_LOADINDIRECT_U16);
		GET_LABELS(JIT_LOADINDIRECT_I32);
		GET_LABELS(JIT_LOADINDIRECT_U32);
		GET_LABELS(JIT_LOADINDIRECT_I64);
		GET_LABELS(JIT_LOADINDIRECT_I);

		GET_LABELS(JIT_LOADINDIRECT_R32);
		GET_LABELS(JIT_LOADINDIRECT_R64);
		GET_LABELS(JIT_LOADINDIRECT_REF);
		GET_LABELS(JIT_STOREINDIRECT_REF);
		GET_LABELS(JIT_STOREINDIRECT_U8);
		GET_LABELS(JIT_STOREINDIRECT_U16);
		GET_LABELS(JIT_STOREINDIRECT_U32);
		GET_LABELS(JIT_STOREINDIRECT_U64);
		GET_LABELS(JIT_STOREINDIRECT_R32);
		GET_LABELS(JIT_STOREINDIRECT_R64);

		GET_LABELS_DYNAMIC(JIT_CONV_I32_I32, 4);
		GET_LABELS_DYNAMIC(JIT_CONV_I32_U32, 4);
		GET_LABELS_DYNAMIC(JIT_CONV_I32_I64, 0);
		GET_LABELS_DYNAMIC(JIT_CONV_I32_U64, 0);
		GET_LABELS_DYNAMIC(JIT_CONV_I32_R32, 0);
		GET_LABELS_DYNAMIC(JIT_CONV_I32_R64, 0);
		GET_LABELS_DYNAMIC(JIT_CONV_U32_I32, 4);
		GET_LABELS_DYNAMIC(JIT_CONV_U32_U32, 4);
		GET_LABELS_DYNAMIC(JIT_CONV_U32_I64, 0);
		GET_LABELS_DYNAMIC(JIT_CONV_U32_U64, 0);
		GET_LABELS_DYNAMIC(JIT_CONV_U32_R32, 0);
		GET_LABELS_DYNAMIC(JIT_CONV_U32_R64, 0);
		GET_LABELS_DYNAMIC(JIT_CONV_I64_I32, 4);
		GET_LABELS_DYNAMIC(JIT_CONV_I64_U32, 4);
		GET_LABELS_DYNAMIC(JIT_CONV_I64_U64, 0);
		GET_LABELS_DYNAMIC(JIT_CONV_I64_R32, 0);
		GET_LABELS_DYNAMIC(JIT_CONV_I64_R64, 0);
		GET_LABELS_DYNAMIC(JIT_CONV_U64_I32, 4);
		GET_LABELS_DYNAMIC(JIT_CONV_U64_U32, 4);
		GET_LABELS_DYNAMIC(JIT_CONV_U64_I64, 0);
		GET_LABELS_DYNAMIC(JIT_CONV_U64_R32, 0);
		GET_LABELS_DYNAMIC(JIT_CONV_U64_R64, 0);
		GET_LABELS_DYNAMIC(JIT_CONV_R32_I32, 4);
		GET_LABELS_DYNAMIC(JIT_CONV_R32_U32, 4);
		GET_LABELS_DYNAMIC(JIT_CONV_R32_I64, 0);
		GET_LABELS_DYNAMIC(JIT_CONV_R32_U64, 0);
		GET_LABELS_DYNAMIC(JIT_CONV_R32_R32, 0);
		GET_LABELS_DYNAMIC(JIT_CONV_R32_R64, 0);
		GET_LABELS_DYNAMIC(JIT_CONV_R64_I32, 4);
		GET_LABELS_DYNAMIC(JIT_CONV_R64_U32, 4);
		GET_LABELS_DYNAMIC(JIT_CONV_R64_I64, 0);
		GET_LABELS_DYNAMIC(JIT_CONV_R64_U64, 0);
		GET_LABELS_DYNAMIC(JIT_CONV_R64_R32, 0);
		GET_LABELS_DYNAMIC(JIT_CONV_R64_R64, 0);

		GET_LABELS(JIT_STORE_ELEMENT_32);
		GET_LABELS(JIT_STORE_ELEMENT_PTR);
		GET_LABELS(JIT_LOAD_ELEMENT_PTR);
		GET_LABELS(JIT_STORE_ELEMENT_64);

		GET_LABELS(JIT_LOAD_ELEMENT_I8);
		GET_LABELS(JIT_LOAD_ELEMENT_U8);
		GET_LABELS(JIT_LOAD_ELEMENT_I16);
		GET_LABELS(JIT_LOAD_ELEMENT_U16);
		GET_LABELS(JIT_LOAD_ELEMENT_I32);
		GET_LABELS(JIT_LOAD_ELEMENT_U32);
		GET_LABELS(JIT_LOAD_ELEMENT_I64);
		GET_LABELS(JIT_LOAD_ELEMENT_R32);
		GET_LABELS(JIT_LOAD_ELEMENT_R64);

		GET_LABELS_DYNAMIC(JIT_ADD_OVF_I64I64, 0);
		GET_LABELS_DYNAMIC(JIT_ADD_OVF_UN_I64I64, 0);
		GET_LABELS_DYNAMIC(JIT_SUB_OVF_I64I64, 0);
		GET_LABELS_DYNAMIC(JIT_SUB_OVF_UN_I64I64, 0);
		GET_LABELS_DYNAMIC(JIT_MUL_OVF_I64I64, 0);
		GET_LABELS_DYNAMIC(JIT_MUL_OVF_UN_I64I64, 0);
		GET_LABELS_DYNAMIC(JIT_CONV_OVF_CHECK, 4);
		GET_LABELS_DYNAMIC(JIT_REM_F32F32, 0);
		GET_LABELS_DYNAMIC(JIT_REM_F64F64, 0);
		GET_LABELS_DYNAMIC(JIT_ADD_I64I64, 0);
		GET_LABELS_DYNAMIC(JIT_SUB_I64I64, 0);
		GET_LABELS_DYNAMIC(JIT_MUL_I64I64, 0);
		GET_LABELS_DYNAMIC(JIT_DIV_I64I64, 0);
		GET_LABELS_DYNAMIC(JIT_DIV_UN_I64I64, 0);
		GET_LABELS_DYNAMIC(JIT_REM_I64I64, 0);
		GET_LABELS_DYNAMIC(JIT_REM_UN_I64I64, 0);
		GET_LABELS_DYNAMIC(JIT_AND_I64I64, 0);
		GET_LABELS_DYNAMIC(JIT_OR_I64I64, 0);
		GET_LABELS_DYNAMIC(JIT_XOR_I64I64, 0);

		GET_LABELS_DYNAMIC(JIT_CEQ_F32F32, 0);
		GET_LABELS_DYNAMIC(JIT_CGT_F32F32, 0);
		GET_LABELS_DYNAMIC(JIT_CGT_UN_F32F32, 0);
		GET_LABELS_DYNAMIC(JIT_CLT_UN_F32F32, 0);
		GET_LABELS_DYNAMIC(JIT_CGT_UN_F64F64, 0);
		GET_LABELS_DYNAMIC(JIT_CLT_UN_F64F64, 0);
		GET_LABELS_DYNAMIC(JIT_CLT_F32F32, 0);
		GET_LABELS_DYNAMIC(JIT_CEQ_F64F64, 0);
		GET_LABELS_DYNAMIC(JIT_CGT_F64F64, 0);
		GET_LABELS_DYNAMIC(JIT_CLT_F64F64, 0);

		GET_LABELS_DYNAMIC(JIT_ADD_F32F32, 0);
		GET_LABELS_DYNAMIC(JIT_ADD_F64F64, 0);
		GET_LABELS_DYNAMIC(JIT_SUB_F32F32, 0);
		GET_LABELS_DYNAMIC(JIT_SUB_F64F64, 0);
		GET_LABELS_DYNAMIC(JIT_MUL_F32F32, 0);
		GET_LABELS_DYNAMIC(JIT_MUL_F64F64, 0);
		GET_LABELS_DYNAMIC(JIT_DIV_F32F32, 0);
		GET_LABELS_DYNAMIC(JIT_DIV_F64F64, 0);

		GET_LABELS_DYNAMIC(JIT_LOAD_I4_M1, 0);
		GET_LABELS_DYNAMIC(JIT_LOAD_I4_0, 0);
		GET_LABELS_DYNAMIC(JIT_LOAD_I4_1, 0);
		GET_LABELS_DYNAMIC(JIT_LOAD_I4_2, 0);

		GET_LABELS_DYNAMIC(JIT_LOADFIELD_4, 4);

		return 0;
	}

#ifdef DIAG_OPCODE_TIMES
	U64 opcodeStartTime = rdtsc();
	U32 realOp;
#endif

	LOAD_METHOD_STATE();

	GO_NEXT();

noCode:
	Crash("No code for op-code");

JIT_NOP_start:
JIT_CONV_R32_R32_start:
JIT_CONV_R64_R64_start:
JIT_CONV_I64_U64_start:
JIT_CONV_U64_I64_start:
	OPCODE_USE(JIT_NOP);
JIT_NOP_end:
JIT_CONV_R32_R32_end:
JIT_CONV_R64_R64_end:
JIT_CONV_I64_U64_end:
JIT_CONV_U64_I64_end:
JIT_GoNext_start:
	GO_NEXT();
JIT_GoNext_end:

JIT_LOAD_NULL_start:
	OPCODE_USE(JIT_LOAD_NULL);
	PUSH_O(NULL);
JIT_LOAD_NULL_end:
	GO_NEXT();

JIT_DUP_4_start:
	OPCODE_USE(JIT_DUP_4);
	DUP4();
JIT_DUP_4_end:
	GO_NEXT();

JIT_DUP_8_start:
	OPCODE_USE(JIT_DUP_8);
	DUP8();
JIT_DUP_8_end:
	GO_NEXT();

JIT_DUP_GENERAL_start:
	OPCODE_USE(JIT_DUP_GENERAL);
	{
		U32 dupSize = GET_OP();
		DUP(dupSize);
	}
JIT_DUP_GENERAL_end:
	GO_NEXT();

JIT_POP_start:
	OPCODE_USE(JIT_POP);
	{
		U32 popSize = GET_OP();
		POP(popSize);
	}
JIT_POP_end:
	GO_NEXT();

JIT_POP_4_start:
	OPCODE_USE(JIT_POP_4);
	POP(4);
JIT_POP_4_end:
	GO_NEXT();

JIT_LOAD_I32_start:
JIT_LOAD_F32_start:
	OPCODE_USE(JIT_LOAD_I32);
	{
		I32 value = GET_OP();
		PUSH_U32(value);
	}
JIT_LOAD_I32_end:
JIT_LOAD_F32_end:
	GO_NEXT();

JIT_LOAD_I4_M1_start:
	OPCODE_USE(JIT_LOAD_I4_M1);
	PUSH_U32(-1);
JIT_LOAD_I4_M1_end:
	GO_NEXT();

JIT_LOAD_I4_0_start:
	OPCODE_USE(JIT_LOAD_I4_0);
	PUSH_U32(0);
JIT_LOAD_I4_0_end:
	GO_NEXT();

JIT_LOAD_I4_1_start:
	OPCODE_USE(JIT_LOAD_I4_1);
	PUSH_U32(1);
JIT_LOAD_I4_1_end:
	GO_NEXT();

JIT_LOAD_I4_2_start:
	OPCODE_USE(JIT_LOAD_I4_2);
	PUSH_U32(2);
JIT_LOAD_I4_2_end:
	GO_NEXT();

JIT_LOAD_I64_start:
JIT_LOAD_F64_start:
	OPCODE_USE(JIT_LOAD_I64);
	{
		U64 value;
		if (sizeof(tOpWord) >= sizeof(U64)) {
			value = (U64)GET_OP();      // one word holds all 64 bits
		} else {
			value = *(U64*)pCurOp;      // two words, low half first
			pCurOp += 2;
		}
		PUSH_U64(value);
	}
JIT_LOAD_I64_end:
JIT_LOAD_F64_end:
	GO_NEXT();

JIT_LOADPARAMLOCAL_INT32_start:
JIT_LOADPARAMLOCAL_F32_start:
	OPCODE_USE(JIT_LOADPARAMLOCAL_INT32);
	{
		U32 ofs = GET_OP();
		U32 value = PARAMLOCAL_U32(ofs);
		PUSH_U32(value);
	}
JIT_LOADPARAMLOCAL_INT32_end:
JIT_LOADPARAMLOCAL_F32_end:
	GO_NEXT();

// References, native ints and managed pointers are pointer-sized. (These used to share the 4-byte handler
// above, marked "only on 32-bit".)
JIT_LOADPARAMLOCAL_O_start:
JIT_LOADPARAMLOCAL_INTNATIVE_start:
JIT_LOADPARAMLOCAL_PTR_start:
	OPCODE_USE(JIT_LOADPARAMLOCAL_PTR);
	{
		U32 ofs = GET_OP();
		PTR value = *(PTR*)(pParamsLocals + ofs);
		PUSH_PTR(value);
	}
JIT_LOADPARAMLOCAL_O_end:
JIT_LOADPARAMLOCAL_INTNATIVE_end:
JIT_LOADPARAMLOCAL_PTR_end:
	GO_NEXT();

JIT_LOADPARAMLOCAL_INT64_start:
JIT_LOADPARAMLOCAL_F64_start:
	OPCODE_USE(JIT_LOADPARAMLOCAL_INT64);
	{
		U32 ofs = GET_OP();
		U64 value = PARAMLOCAL_U64(ofs);
		PUSH_U64(value);
	}
JIT_LOADPARAMLOCAL_INT64_end:
JIT_LOADPARAMLOCAL_F64_end:
	GO_NEXT();

JIT_LOADPARAMLOCAL_VALUETYPE_start:
	OPCODE_USE(JIT_LOADPARAMLOCAL_VALUETYPE);
	{
		tMD_TypeDef *pTypeDef;
		U32 ofs;
		PTR pMem;

		ofs = GET_OP();
		pTypeDef = (tMD_TypeDef*)GET_OP();
		pMem = pParamsLocals + ofs;
		PUSH_VALUETYPE(pMem, pTypeDef->stackSize, pTypeDef->stackSize);
	}
JIT_LOADPARAMLOCAL_VALUETYPE_end:
	GO_NEXT();

JIT_LOADPARAMLOCAL_0_start:
	OPCODE_USE(JIT_LOADPARAMLOCAL_0);
	PUSH_U32(PARAMLOCAL_U32(0));
JIT_LOADPARAMLOCAL_0_end:
	GO_NEXT();

JIT_LOADPARAMLOCAL_1_start:
	OPCODE_USE(JIT_LOADPARAMLOCAL_1);
	PUSH_U32(PARAMLOCAL_U32(4));
JIT_LOADPARAMLOCAL_1_end:
	GO_NEXT();

JIT_LOADPARAMLOCAL_2_start:
	OPCODE_USE(JIT_LOADPARAMLOCAL_2);
	PUSH_U32(PARAMLOCAL_U32(8));
JIT_LOADPARAMLOCAL_2_end:
	GO_NEXT();

JIT_LOADPARAMLOCAL_3_start:
	OPCODE_USE(JIT_LOADPARAMLOCAL_3);
	PUSH_U32(PARAMLOCAL_U32(12));
JIT_LOADPARAMLOCAL_3_end:
	GO_NEXT();

JIT_LOADPARAMLOCAL_4_start:
	OPCODE_USE(JIT_LOADPARAMLOCAL_4);
	PUSH_U32(PARAMLOCAL_U32(16));
JIT_LOADPARAMLOCAL_4_end:
	GO_NEXT();

JIT_LOADPARAMLOCAL_5_start:
	OPCODE_USE(JIT_LOADPARAMLOCAL_5);
	PUSH_U32(PARAMLOCAL_U32(20));
JIT_LOADPARAMLOCAL_5_end:
	GO_NEXT();

JIT_LOADPARAMLOCAL_6_start:
	OPCODE_USE(JIT_LOADPARAMLOCAL_6);
	PUSH_U32(PARAMLOCAL_U32(24));
JIT_LOADPARAMLOCAL_6_end:
	GO_NEXT();

JIT_LOADPARAMLOCAL_7_start:
	OPCODE_USE(JIT_LOADPARAMLOCAL_7);
	PUSH_U32(PARAMLOCAL_U32(28));
JIT_LOADPARAMLOCAL_7_end:
	GO_NEXT();

JIT_LOAD_PARAMLOCAL_ADDR_start:
	OPCODE_USE(JIT_LOAD_PARAMLOCAL_ADDR);
	{
		U32 ofs = GET_OP();
		PTR pMem = pParamsLocals + ofs;
		PUSH_PTR(pMem);
	}
JIT_LOAD_PARAMLOCAL_ADDR_end:
	GO_NEXT();

JIT_STOREPARAMLOCAL_INT32_start:
JIT_STOREPARAMLOCAL_F32_start:
	OPCODE_USE(JIT_STOREPARAMLOCAL_INT32);
	{
		U32 ofs = GET_OP();
		U32 value = POP_U32();
		PARAMLOCAL_U32(ofs) = value;
	}
JIT_STOREPARAMLOCAL_INT32_end:
JIT_STOREPARAMLOCAL_F32_end:
	GO_NEXT();

JIT_STOREPARAMLOCAL_O_start:
JIT_STOREPARAMLOCAL_INTNATIVE_start:
JIT_STOREPARAMLOCAL_PTR_start:
	OPCODE_USE(JIT_STOREPARAMLOCAL_PTR);
	{
		U32 ofs = GET_OP();
		PTR value = POP_PTR();
		*(PTR*)(pParamsLocals + ofs) = value;
	}
JIT_STOREPARAMLOCAL_O_end:
JIT_STOREPARAMLOCAL_INTNATIVE_end:
JIT_STOREPARAMLOCAL_PTR_end:
	GO_NEXT();

JIT_STOREPARAMLOCAL_INT64_start:
JIT_STOREPARAMLOCAL_F64_start:
	OPCODE_USE(JIT_STOREPARAMLOCAL_INT64);
	{
		U32 ofs = GET_OP();
		U64 value = POP_U64();
		PARAMLOCAL_U64(ofs) = value;
	}
JIT_STOREPARAMLOCAL_INT64_end:
JIT_STOREPARAMLOCAL_F64_end:
	GO_NEXT();

JIT_STOREPARAMLOCAL_VALUETYPE_start:
	OPCODE_USE(JIT_STOREPARAMLOCAL_VALUETYPE);
	{
		tMD_TypeDef *pTypeDef;
		U32 ofs;
		PTR pMem;

		ofs = GET_OP();
		pTypeDef = (tMD_TypeDef*)GET_OP();
		pMem = pParamsLocals + ofs;
		POP_VALUETYPE(pMem, pTypeDef->stackSize, pTypeDef->stackSize);
	}
JIT_STOREPARAMLOCAL_VALUETYPE_end:
	GO_NEXT();

JIT_STOREPARAMLOCAL_0_start:
	OPCODE_USE(JIT_STOREPARAMLOCAL_0);
	PARAMLOCAL_U32(0) = POP_U32();
JIT_STOREPARAMLOCAL_0_end:
	GO_NEXT();

JIT_STOREPARAMLOCAL_1_start:
	OPCODE_USE(JIT_STOREPARAMLOCAL_1);
	PARAMLOCAL_U32(4) = POP_U32();
JIT_STOREPARAMLOCAL_1_end:
	GO_NEXT();

JIT_STOREPARAMLOCAL_2_start:
	OPCODE_USE(JIT_STOREPARAMLOCAL_2);
	PARAMLOCAL_U32(8) = POP_U32();
JIT_STOREPARAMLOCAL_2_end:
	GO_NEXT();

JIT_STOREPARAMLOCAL_3_start:
	OPCODE_USE(JIT_STOREPARAMLOCAL_3);
	PARAMLOCAL_U32(12) = POP_U32();
JIT_STOREPARAMLOCAL_3_end:
	GO_NEXT();

JIT_STOREPARAMLOCAL_4_start:
	OPCODE_USE(JIT_STOREPARAMLOCAL_4);
	PARAMLOCAL_U32(16) = POP_U32();
JIT_STOREPARAMLOCAL_4_end:
	GO_NEXT();

JIT_STOREPARAMLOCAL_5_start:
	OPCODE_USE(JIT_STOREPARAMLOCAL_5);
	PARAMLOCAL_U32(20) = POP_U32();
JIT_STOREPARAMLOCAL_5_end:
	GO_NEXT();

JIT_STOREPARAMLOCAL_6_start:
	OPCODE_USE(JIT_STOREPARAMLOCAL_6);
	PARAMLOCAL_U32(24) = POP_U32();
JIT_STOREPARAMLOCAL_6_end:
	GO_NEXT();

JIT_STOREPARAMLOCAL_7_start:
	OPCODE_USE(JIT_STOREPARAMLOCAL_7);
	PARAMLOCAL_U32(28) = POP_U32();
JIT_STOREPARAMLOCAL_7_end:
	GO_NEXT();

JIT_LOADINDIRECT_I8_start:
	OPCODE_USE(JIT_LOADINDIRECT_I8);
	{
		PTR pMem = POP_PTR();
		if (pMem == NULL) {
			THROW_NULLREF();
		}
		PUSH_U32(IsPackedNarrowElement(pMem) ? (U32)(I32)*(I8*)pMem : *(U32*)pMem);
	}
JIT_LOADINDIRECT_I8_end:
	GO_NEXT();

JIT_LOADINDIRECT_U8_start:
	OPCODE_USE(JIT_LOADINDIRECT_U8);
	{
		PTR pMem = POP_PTR();
		if (pMem == NULL) {
			THROW_NULLREF();
		}
		PUSH_U32(IsPackedNarrowElement(pMem) ? (U32)*(U8*)pMem : *(U32*)pMem);
	}
JIT_LOADINDIRECT_U8_end:
	GO_NEXT();

JIT_LOADINDIRECT_I16_start:
	OPCODE_USE(JIT_LOADINDIRECT_I16);
	{
		PTR pMem = POP_PTR();
		if (pMem == NULL) {
			THROW_NULLREF();
		}
		PUSH_U32(IsPackedNarrowElement(pMem) ? (U32)(I32)*(I16*)pMem : *(U32*)pMem);
	}
JIT_LOADINDIRECT_I16_end:
	GO_NEXT();

JIT_LOADINDIRECT_U16_start:
	OPCODE_USE(JIT_LOADINDIRECT_U16);
	{
		PTR pMem = POP_PTR();
		if (pMem == NULL) {
			THROW_NULLREF();
		}
		PUSH_U32(IsPackedNarrowElement(pMem) ? (U32)*(U16*)pMem : *(U32*)pMem);
	}
JIT_LOADINDIRECT_U16_end:
	GO_NEXT();

JIT_LOADINDIRECT_I32_start:
JIT_LOADINDIRECT_U32_start:
JIT_LOADINDIRECT_R32_start:
	OPCODE_USE(JIT_LOADINDIRECT_U32);
	{
		PTR pMem = POP_PTR();
		U32 value;
		if (pMem == NULL) {
			THROW_NULLREF();
		}
		value = *(U32*)pMem;
		PUSH_U32(value);
	}
JIT_LOADINDIRECT_I32_end:
JIT_LOADINDIRECT_U32_end:
JIT_LOADINDIRECT_R32_end:
	GO_NEXT();

JIT_LOADINDIRECT_REF_start:
	OPCODE_USE(JIT_LOADINDIRECT_REF);
	{
		PTR pMem = POP_PTR();
		if (pMem == NULL) {
			THROW_NULLREF();
		}
		PUSH_PTR(*(PTR*)pMem);
	}
JIT_LOADINDIRECT_REF_end:
	GO_NEXT();

JIT_LOADINDIRECT_I_start:
	OPCODE_USE(JIT_LOADINDIRECT_I);
	{
		PTR pMem = POP_PTR();
		if (pMem == NULL) {
			THROW_NULLREF();
		}
		PUSH_PTR(*(PTR*)pMem);
	}
JIT_LOADINDIRECT_I_end:
	GO_NEXT();

JIT_LOADINDIRECT_R64_start:
JIT_LOADINDIRECT_I64_start:
	OPCODE_USE(JIT_LOADINDIRECT_I64);
	{
		PTR pMem = POP_PTR();
		U64 value;
		if (pMem == NULL) {
			THROW_NULLREF();
		}
		value = *(U64*)pMem;
		PUSH_U64(value);
	}
JIT_LOADINDIRECT_R64_end:
JIT_LOADINDIRECT_I64_end:
	GO_NEXT();

JIT_STOREINDIRECT_U8_start:
	OPCODE_USE(JIT_STOREINDIRECT_U8);
	{
		U32 value = POP_U32(); // The value to store
		PTR pMem = POP_PTR(); // The address to store to
		if (pMem == NULL) {
			THROW_NULLREF();
		}
		if (IsPackedNarrowElement(pMem)) {
			*(U8*)pMem = (U8)value;
		} else {
			*(U32*)pMem = value;
		}
	}
JIT_STOREINDIRECT_U8_end:
	GO_NEXT();

JIT_STOREINDIRECT_U16_start:
	OPCODE_USE(JIT_STOREINDIRECT_U16);
	{
		U32 value = POP_U32();
		PTR pMem = POP_PTR();
		if (pMem == NULL) {
			THROW_NULLREF();
		}
		if (IsPackedNarrowElement(pMem)) {
			*(U16*)pMem = (U16)value;
		} else {
			*(U32*)pMem = value;
		}
	}
JIT_STOREINDIRECT_U16_end:
	GO_NEXT();

JIT_STOREINDIRECT_U32_start:
JIT_STOREINDIRECT_R32_start:
	OPCODE_USE(JIT_STOREINDIRECT_U32);
	{
		U32 value = POP_U32(); // The value to store
		PTR pMem = POP_PTR(); // The address to store to
		if (pMem == NULL) {
			THROW_NULLREF();
		}
		*(U32*)pMem = value;
	}
JIT_STOREINDIRECT_U32_end:
JIT_STOREINDIRECT_R32_end:
	GO_NEXT();

JIT_STOREINDIRECT_REF_start:
	OPCODE_USE(JIT_STOREINDIRECT_REF);
	{
		PTR value = POP_PTR(); // The reference (or native int) to store
		PTR pMem = POP_PTR(); // The address to store to
		if (pMem == NULL) {
			THROW_NULLREF();
		}
		*(PTR*)pMem = value;
	}
JIT_STOREINDIRECT_REF_end:
	GO_NEXT();

JIT_STOREINDIRECT_U64_start:
JIT_STOREINDIRECT_R64_start:
	OPCODE_USE(JIT_STOREINDIRECT_U64);
	{
		U64 value = POP_U64();
		PTR pMem = POP_PTR();
		if (pMem == NULL) {
			THROW_NULLREF();
		}
		*(U64*)pMem = value;
	}
JIT_STOREINDIRECT_U64_end:
JIT_STOREINDIRECT_R64_end:
	GO_NEXT();

JIT_STORE_OBJECT_VALUETYPE_start:
	OPCODE_USE(JIT_STORE_OBJECT_VALUETYPE);
	{
		U32 size = GET_OP(); // The size, in bytes, of the value-type to store
		U32 memSize = (size<4)?4:size;
		PTR pMem = pCurEvalStack - memSize - sizeof(void*);
		POP_VALUETYPE(*(void**)pMem, size, memSize);
		POP(sizeof(void*));   // the destination address (this was POP(4): a pointer is 8 bytes on a 64-bit target)
	}
JIT_STORE_OBJECT_VALUETYPE_end:
	GO_NEXT();

JIT_CALL_PINVOKE_start:
	OPCODE_USE(JIT_CALL_PINVOKE);
	{
		tJITCallPInvoke *pCallPInvoke;
		U32 res;

		pCallPInvoke = (tJITCallPInvoke*)(pCurOp - 1);
		res = PInvoke_Call(pCallPInvoke, pParamsLocals, pCurrentMethodState->pEvalStack, pThread);
		pCurrentMethodState->stackOfs = res;
	}
	goto JIT_RETURN_start;
JIT_CALL_PINVOKE_end:

JIT_CALL_NATIVE_start:
	OPCODE_USE(JIT_CALL_NATIVE);
	{
		tJITCallNative *pCallNative;
		PTR pThis;
		U32 thisOfs;
		tAsyncCall *pAsync;

		//pCallNative = (tJITCallNative*)&(pJIT->pOps[pCurrentMethodState->ipOffset - 1]);
		pCallNative = (tJITCallNative*)(pCurOp - 1);
		if (METHOD_ISSTATIC(pCallNative->pMethodDef)) {
			pThis = NULL;
			thisOfs = 0;
		} else {
			pThis = *(PTR*)pCurrentMethodState->pParamsLocals;
			thisOfs = sizeof(void*);
		}
		// Internal constructors MUST leave the newly created object in the return value
		// (ie on top of the evaluation stack)
		pAsync = pCallNative->fn(pThis, pCurrentMethodState->pParamsLocals + thisOfs, pCurrentMethodState->pEvalStack);
		if (pAsync != NULL) {
			// Save the method state
			SAVE_METHOD_STATE();
			// Change the IP pointer to point to the return instruction
			pCurrentMethodState->ipOffset = 3;
			// Handle special async codes
			if (pAsync == ASYNC_LOCK_EXIT) {
				return THREAD_STATUS_LOCK_EXIT;
			}
			// Set the async in the thread
			pThread->pAsync = pAsync;
			return THREAD_STATUS_ASYNC;
		}
	}
	// fall-through
JIT_CALL_NATIVE_end:

JIT_RETURN_start:
	OPCODE_USE(JIT_RETURN);
	//printf("Returned from %s() to %s()\n", pCurrentMethodState->pMethod->name, (pCurrentMethodState->pCaller)?pCurrentMethodState->pCaller->pMethod->name:"<none>");
	if (pCurrentMethodState->pCaller == NULL) {
		// End of thread!
		{
			tMD_TypeDef *pRetType = pCurrentMethodState->pMethod->pReturnType;
			if (pRetType != NULL && pRetType->stackSize <= sizeof(Thread_LastReturn)) {
				// (for a native host: the result of any type, not only an int32)
				Thread_LastReturnSize = pRetType->stackSize;
				memcpy(Thread_LastReturn, pCurEvalStack - pRetType->stackSize, pRetType->stackSize);
			} else {
				Thread_LastReturnSize = 0;
			}
		}
		if (pCurrentMethodState->pMethod->pReturnType == types[TYPE_SYSTEM_INT32]) {
			// If function returned an int32, then make it the thread exit-value
			pThread->threadExitValue = (I32)POP_U32();
		}
		return THREAD_STATUS_EXIT;
	}
	// Make u32Value the number of bytes of the return value from the function
	if (pCurrentMethodState->pMethod->pReturnType != NULL) {
		u32Value = pCurrentMethodState->pMethod->pReturnType->stackSize;
	} else if (pCurrentMethodState->isInternalNewObjCall) {
		u32Value = sizeof(void*);
	} else {
		u32Value = 0;
	}
	pMem = pCurrentMethodState->pEvalStack;
	{
		tMethodState *pOldMethodState = pCurrentMethodState;
		pThread->pCurrentMethodState = pCurrentMethodState->pCaller;
		LOAD_METHOD_STATE();
		// Copy return value to callers evaluation stack
		if (u32Value > 0) {
			SmallCopy(pCurEvalStack, pMem, u32Value);   // the two evaluation stacks are separate blocks
			pCurEvalStack += u32Value;
		}
		// Delete the current method state and go back to callers method state
#if FAST_CALL_PATH
		if (pOldMethodState->finalizerThis == NULL && pOldMethodState->pDelegateParams == NULL) {
			// the usual frame: freeing it is giving back its space on the thread stack
			tThreadStack *pOldStack = pThread->pThreadStack;
			pOldStack->ofs = (U32)((unsigned char*)pOldMethodState - pOldStack->memory);
		} else {
			MethodState_Delete(pThread, &pOldMethodState);
		}
#else
		MethodState_Delete(pThread, &pOldMethodState);
#endif
	}
	if (pCurrentMethodState->pNextDelegate == NULL) {
		GO_NEXT();
	}
	// Fall-through if more delegate methods to invoke
JIT_RETURN_end:

JIT_INVOKE_DELEGATE_start:
	OPCODE_USE(JIT_INVOKE_DELEGATE);
	{
		tMD_MethodDef *pDelegateMethod, *pCallMethod;
		void *pDelegate;
		HEAP_PTR pDelegateThis;
		tMethodState *pCallMethodState;
		U32 ofs;

		if (pCurrentMethodState->pNextDelegate == NULL) {
			// First delegate, so get the Invoke() method defined within the delegate class
			pDelegateMethod = (tMD_MethodDef*)GET_OP();
			// Take the params off the stack. This is the pointer to the tDelegate & params
			//pCurrentMethodState->stackOfs -= pDelegateMethod->parameterStackSize;
			pCurEvalStack -= pDelegateMethod->parameterStackSize;
			{
				// The usual delegate has one target. Then the arguments are still on the evaluation stack, just above the delegate, and
				// the target's frame is filled from there: nothing is allocated or copied twice. (A multicast delegate needs its
				// arguments again for each target, so those are kept, below.)
				void *pSingle = *(void**)pCurEvalStack;
				if (pSingle != NULL) {
					void *pSingleNext;
					HEAP_PTR pSingleThis;
					tMD_MethodDef *pSingleMethod = Delegate_GetMethodAndStore(pSingle, &pSingleThis, &pSingleNext);
					if (pSingleNext == NULL) {
						U32 sofs = (pSingleThis != NULL) ? (U32)sizeof(void*) : 0;
						tMethodState *pSingleState = MethodState_Direct(pThread, pSingleMethod, pCurrentMethodState, 0);
						if (pSingleThis != NULL) {
							*(HEAP_PTR*)pSingleState->pParamsLocals = pSingleThis;
						}
						SmallCopy(pSingleState->pParamsLocals + sofs, pCurEvalStack + sizeof(void*), pSingleMethod->parameterStackSize - sofs);
						CHANGE_METHOD_STATE(pSingleState);
						GO_NEXT();
					}
				}
			}
			// Allocate memory for delegate params (the previous invocation's, if there was one, is finished with: it used to be lost here)
			free(pCurrentMethodState->pDelegateParams);
			pCurrentMethodState->pDelegateParams = malloc(pDelegateMethod->parameterStackSize - sizeof(void*));
			memcpy(
				pCurrentMethodState->pDelegateParams,
				//pCurrentMethodState->pEvalStack + pCurrentMethodState->stackOfs + sizeof(void*),
				pCurEvalStack + sizeof(void*),
				pDelegateMethod->parameterStackSize - sizeof(void*));
			// Get the actual delegate heap pointer
			pDelegate = *(void**)pCurEvalStack;
		} else {
			pDelegateMethod = Delegate_GetMethod(pCurrentMethodState->pNextDelegate);
			if (pDelegateMethod->pReturnType != NULL) {
				pCurEvalStack -= pDelegateMethod->pReturnType->stackSize;
			}
			// Get the actual delegate heap pointer
			pDelegate = pCurrentMethodState->pNextDelegate;
		}
		if (pDelegate == NULL) {
			THROW(types[TYPE_SYSTEM_NULLREFERENCEEXCEPTION]);
		}
		// Get the real method to call; the target of the delegate.
		pCallMethod = Delegate_GetMethodAndStore(pDelegate, &pDelegateThis, &pCurrentMethodState->pNextDelegate);
		// Set up the call method state for the call.
		pCallMethodState = MethodState_Direct(pThread, pCallMethod, pCurrentMethodState, 0);
		if (pDelegateThis != NULL) {
			*(HEAP_PTR*)pCallMethodState->pParamsLocals = pDelegateThis;
			ofs = sizeof(void*);
		} else {
			ofs = 0;
		}
		memcpy(pCallMethodState->pParamsLocals + ofs,
			pCurrentMethodState->pDelegateParams,
			pCallMethod->parameterStackSize - ofs);
		CHANGE_METHOD_STATE(pCallMethodState);
	}
JIT_INVOKE_DELEGATE_end:
	GO_NEXT();

JIT_INVOKE_SYSTEM_REFLECTION_METHODBASE_start:
	OPCODE_USE(JIT_INVOKE_SYSTEM_REFLECTION_METHODBASE);
	{
		// Get the reference to MethodBase.Invoke
		tMD_MethodDef *pInvokeMethod = (tMD_MethodDef*)GET_OP();

		// Take the MethodBase.Invoke params off the stack.
		pCurEvalStack -= pInvokeMethod->parameterStackSize;

		// Get a pointer to the MethodBase instance (e.g., a MethodInfo or ConstructorInfo),
		// and from that, determine which method we're going to invoke
		tMethodBase *pMethodBase = *(tMethodBase**)pCurEvalStack;
		tMD_MethodDef *pCallMethod = pMethodBase->methodDef;

		// Store the return type so that JIT_REFLECTION_DYNAMICALLY_BOX_RETURN_VALUE can
		// interpret the stack after the invocation
		pCurrentMethodState->pReflectionInvokeReturnType = pCallMethod->pReturnType;

		// Get the 'this' pointer for the call and the params array
		PTR invocationThis = (PTR)(*(tMethodBase**)(pCurEvalStack + sizeof(HEAP_PTR)));
		HEAP_PTR invocationParamsArray = *(HEAP_PTR*)(pCurEvalStack + sizeof(HEAP_PTR) + sizeof(PTR));		

		// Put the new 'this' on the stack
		PTR pPrevEvalStack = pCurEvalStack;
		PUSH_PTR(invocationThis);

		// Put any other params on the stack
		if (invocationParamsArray != NULL) {
			U32 invocationParamsArrayLength = SystemArray_GetLength(invocationParamsArray);
			PTR invocationParamsArrayElements = SystemArray_GetElements(invocationParamsArray);
			for (U32 paramIndex = 0; paramIndex < invocationParamsArrayLength; paramIndex++) {
				HEAP_PTR currentParam = (HEAP_PTR)(((U32*)(invocationParamsArrayElements))[paramIndex]);
				if (currentParam == NULL) {
					PUSH_O(NULL);
				} else {
					tMD_TypeDef *currentParamType = Heap_GetType(currentParam);

					if (Type_IsValueType(currentParamType)) {
						PUSH_VALUETYPE(currentParam, currentParamType->stackSize, currentParamType->stackSize);
					} else {
						PUSH_O(currentParam);
					}
				}
			}
		}
		pCurEvalStack = pPrevEvalStack;

		// Change interpreter state so we continue execution inside the method being invoked
		tMethodState *pCallMethodState = MethodState_Direct(pThread, pCallMethod, pCurrentMethodState, 0);
		memcpy(pCallMethodState->pParamsLocals, pCurEvalStack, pCallMethod->parameterStackSize);
		CHANGE_METHOD_STATE(pCallMethodState);
	}
JIT_INVOKE_SYSTEM_REFLECTION_METHODBASE_end:
	GO_NEXT();

JIT_REFLECTION_DYNAMICALLY_BOX_RETURN_VALUE_start:
	OPCODE_USE(JIT_REFLECTION_DYNAMICALLY_BOX_RETURN_VALUE);
	{
		tMD_TypeDef *pLastInvocationReturnType = pCurrentMethodState->pReflectionInvokeReturnType;
		if (pLastInvocationReturnType == NULL) {
			// It was a void method, so it won't have put anything on the stack. We need to put
			// a null value there as a return value, because MethodBase.Invoke isn't void.
			PUSH_O(NULL);
		} else if (Type_IsValueType(pLastInvocationReturnType)) {
			// For value types, remove the raw value data from the stack and replace it with a
			// boxed copy, because MethodBase.Invoke returns object.
			HEAP_PTR heapPtr = Heap_AllocType(pLastInvocationReturnType);
			POP_VALUETYPE(heapPtr, pLastInvocationReturnType->stackSize, pLastInvocationReturnType->stackSize);
			PUSH_O(heapPtr);
		}
	}

JIT_REFLECTION_DYNAMICALLY_BOX_RETURN_VALUE_end:
	GO_NEXT_CHECK();

JIT_DEREF_CALLVIRT_start:
	op = JIT_DEREF_CALLVIRT;
	goto allCallStart;
JIT_BOX_CALLVIRT_start:
	op = JIT_BOX_CALLVIRT;
	goto allCallStart;
#if FAST_CALL_PATH
// The common calls, call and callvirt: the target is known (callvirt finds it in the vtable), its frame is made right here out of the
// thread stack, and only its locals are cleared (the parameters are written by the copy of the arguments). Anything unusual (a
// method not yet filled in or compiled, a null `this`) is left to the general code below, which does all of it as it always did.
#define FAST_CALL(pM) do { \
	tMD_MethodDef *fm_ = (pM); \
	tJITted *fj_ = fm_->pJITted; \
	tThreadStack *fs_ = pThread->pThreadStack; \
	U32 fpsz_ = fm_->parameterStackSize, fls_ = fj_->localsStackSize, fms_ = fj_->maxStack; \
	PTR fblock_ = fs_->memory + fs_->ofs; \
	tMethodState *fnew_ = (tMethodState*)fblock_; \
	fs_->ofs += (U32)sizeof(tMethodState) + fms_ + fpsz_ + fls_; \
	if (fs_->ofs > THREADSTACK_CHUNK_SIZE) { Crash("Thread-local stack is too large"); } \
	fnew_->pEvalStack = fblock_ + sizeof(tMethodState); \
	fnew_->pParamsLocals = fnew_->pEvalStack + fms_; \
	if (fls_ != 0) { SmallZero(fnew_->pParamsLocals + fpsz_, fls_); } \
	pCurEvalStack -= fpsz_; \
	SmallCopy(fnew_->pParamsLocals, pCurEvalStack, fpsz_); \
	fnew_->finalizerThis = NULL; fnew_->nativeEntry = 0; fnew_->pCaller = pCurrentMethodState; \
	fnew_->pMetaData = fm_->pMetaData; fnew_->pMethod = fm_; fnew_->pJIT = fj_; fnew_->ipOffset = 0; fnew_->stackOfs = 0; \
	fnew_->isInternalNewObjCall = 0; fnew_->pNextDelegate = NULL; fnew_->pDelegateParams = NULL; \
	CHANGE_METHOD_STATE(fnew_); \
} while (0)

JIT_CALL_PTR_start: // Note that JIT_CALL_PTR cannot be virtual
JIT_CALL_O_start:
	OPCODE_USE(JIT_CALL_O);
	{
		tMD_MethodDef *pFastMethod = (tMD_MethodDef*)GET_OP();
		if (pFastMethod->isFilled && pFastMethod->pJITted != NULL) {
			FAST_CALL(pFastMethod);
			GO_NEXT_CHECK();
		}
		pCurOp--;                       // not the usual case: the general code reads the operand itself
	}
	op = JIT_CALL_O;
	goto allCallStart;
JIT_CALLVIRT_O_start:
	OPCODE_USE(JIT_CALLVIRT_O);
	{
		tMD_MethodDef *pFastMethod = (tMD_MethodDef*)GET_OP();
		HEAP_PTR pFastThis = *(HEAP_PTR*)(pCurEvalStack - pFastMethod->parameterStackSize);
		if (pFastThis != NULL) {
			if (METHOD_ISVIRTUAL(pFastMethod)) {
				pFastMethod = Heap_GetType(pFastThis)->pVTable[pFastMethod->vTableOfs];
			}
			if (pFastMethod->isFilled && pFastMethod->pJITted != NULL) {
				FAST_CALL(pFastMethod);
				GO_NEXT_CHECK();
			}
		}
		pCurOp--;                       // a null `this`, or a method not ready: the general code does it (and throws)
	}
	op = JIT_CALLVIRT_O;
	goto allCallStart;
#else
JIT_CALL_PTR_start: // Note that JIT_CALL_PTR cannot be virtual
	op = JIT_CALL_PTR;
	goto allCallStart;
JIT_CALLVIRT_O_start:
	op = JIT_CALLVIRT_O;
	goto allCallStart;
JIT_CALL_O_start:
	op = JIT_CALL_O;
	goto allCallStart;
#endif
#if FAST_CALL_PATH
JIT_CALL_INTERFACE_start:
	OPCODE_USE(JIT_CALL_INTERFACE);
	{
		tMD_MethodDef *pFastMethod = (tMD_MethodDef*)GET_OP();
		HEAP_PTR pFastThis = *(HEAP_PTR*)(pCurEvalStack - pFastMethod->parameterStackSize);
		tMD_TypeDef *pFastType, *pFastInterface;
		tMD_MethodDef *pFastTarget = NULL;
		I32 fi;
		if (pFastThis == NULL) {
			// (the general code below did not check this, and crashed)
			THROW(types[TYPE_SYSTEM_NULLREFERENCEEXCEPTION]);
		}
		pFastType = Heap_GetType(pFastThis);
		pFastInterface = pFastMethod->pParentType;
		// searched backwards, as below: if an interface is implemented more than once in the type hierarchy, the most recent one is used
		for (fi = (I32)pFastType->numInterfaces - 1; fi >= 0; fi--) {
			if (pFastType->pInterfaceMaps[fi].pInterface == pFastInterface) {
				if (pFastType->pInterfaceMaps[fi].pVTableLookup != NULL) {
					pFastTarget = pFastType->pVTable[pFastType->pInterfaceMaps[fi].pVTableLookup[pFastMethod->vTableOfs]];
				} else {
					pFastTarget = pFastType->pInterfaceMaps[fi].ppMethodVLookup[pFastMethod->vTableOfs];
				}
				break;
			}
		}
		if (pFastTarget != NULL && pFastTarget->isFilled && pFastTarget->pJITted != NULL) {
			FAST_CALL(pFastTarget);
			GO_NEXT_CHECK();
		}
		pCurOp--;                       // not found, or not ready: the general code does it
	}
	op = JIT_CALL_INTERFACE;
	goto allCallStart;
#else
JIT_CALL_INTERFACE_start:
	op = JIT_CALL_INTERFACE;
	goto allCallStart;
#endif
JIT_CALLI_start:
	op = JIT_CALLI;
allCallStart:
	OPCODE_USE(JIT_CALL_O);
	{
		tMD_MethodDef *pCallMethod;
		tMethodState *pCallMethodState;
		tMD_TypeDef *pBoxCallType;

		if (op == JIT_BOX_CALLVIRT) {
			pBoxCallType = (tMD_TypeDef*)GET_OP();
		}

		if (op == JIT_CALLI) {
			// calli: the target is a method pointer on top of the stack, above the arguments
			pCallMethod = (tMD_MethodDef*)POP_PTR();
			if (pCallMethod == NULL) {
				THROW_NULLREF();
			}
		} else {
			pCallMethod = (tMD_MethodDef*)GET_OP();
		}
		heapPtr = NULL;

		if (op == JIT_BOX_CALLVIRT) {
			// Need to de-ref and box the value-type before calling the function
			// TODO: Will this work on value-types that are not 4 bytes long?
			pMem = pCurEvalStack - pCallMethod->parameterStackSize;
			heapPtr = Heap_Box(pBoxCallType, *(PTR*)pMem);
			*(HEAP_PTR*)pMem = heapPtr;
		} else if (op == JIT_DEREF_CALLVIRT) {
			pMem = pCurEvalStack - pCallMethod->parameterStackSize;
			*(HEAP_PTR*)pMem = **(HEAP_PTR**)pMem;
		}

		// If it's a virtual call then find the real correct method to call
		if (op == JIT_CALLVIRT_O || op == JIT_BOX_CALLVIRT || op == JIT_DEREF_CALLVIRT) {
			tMD_TypeDef *pThisType;
			// Get the actual object that is becoming 'this'
			if (heapPtr == NULL) {
				heapPtr = *(HEAP_PTR*)(pCurEvalStack - pCallMethod->parameterStackSize);
			}
			if (heapPtr == NULL) {
				//Crash("NULL 'this' in Virtual call: %s", Sys_GetMethodDesc(pCallMethod));
				THROW(types[TYPE_SYSTEM_NULLREFERENCEEXCEPTION]);
			}
			pThisType = Heap_GetType(heapPtr);
			if (METHOD_ISVIRTUAL(pCallMethod)) {
				pCallMethod = pThisType->pVTable[pCallMethod->vTableOfs];
			}
		} else if (op == JIT_CALL_INTERFACE) {
			tMD_TypeDef *pInterface, *pThisType;
			U32 vIndex;
			I32 i;

			pInterface = pCallMethod->pParentType;
			// Get the actual object that is becoming 'this'
			heapPtr = *(HEAP_PTR*)(pCurEvalStack - pCallMethod->parameterStackSize);
			pThisType = Heap_GetType(heapPtr);
			// Find the interface mapping on the 'this' type.
			vIndex = 0xffffffff;
			// This must be searched backwards so if an interface is implemented more than
			// once in the type hierarchy, the most recent definition gets called
			for (i=(I32)pThisType->numInterfaces-1; i >= 0; i--) {
				if (pThisType->pInterfaceMaps[i].pInterface == pInterface) {
					// Found the right interface map
					if (pThisType->pInterfaceMaps[i].pVTableLookup != NULL) {
						vIndex = pThisType->pInterfaceMaps[i].pVTableLookup[pCallMethod->vTableOfs];
						break;
					}
					pCallMethod = pThisType->pInterfaceMaps[i].ppMethodVLookup[pCallMethod->vTableOfs];
					goto callMethodSet;
				}
			}
			Assert(vIndex != 0xffffffff);
			pCallMethod = pThisType->pVTable[vIndex];
		}
callMethodSet:
		//printf("Calling method: %s\n", Sys_GetMethodDesc(pCallMethod));
		// Set up the new method state for the called method
		pCallMethodState = MethodState_Direct(pThread, pCallMethod, pCurrentMethodState, 0);
		// Set up the parameter stack for the method being called
		pTempPtr = pCurEvalStack;
		CreateParameters(pCallMethodState->pParamsLocals, pCallMethod, &/*pCurEvalStack*/pTempPtr, NULL);
		pCurEvalStack = pTempPtr;
		// Set up the local variables for the new method state
		CHANGE_METHOD_STATE(pCallMethodState);
	}
JIT_DEREF_CALLVIRT_end:
JIT_BOX_CALLVIRT_end:
JIT_CALL_PTR_end:
JIT_CALLVIRT_O_end:
JIT_CALL_O_end:
JIT_CALL_INTERFACE_end:
JIT_CALLI_end:
	GO_NEXT_CHECK();

JIT_BRANCH_start:
	OPCODE_USE(JIT_BRANCH);
	{
		U32 ofs = GET_OP();
		pCurOp = pOps + ofs;
	}
JIT_BRANCH_end:
	GO_NEXT_CHECK();

JIT_SWITCH_start:
	OPCODE_USE(JIT_SWITCH);
	{
		U32 ofs;
		// The number of jump targets
		U32 numTargets = GET_OP();
		// The jump target selected
		U32 target = POP_U32();
		if (target >= numTargets) {
			// This is not a valid jump target, so fall-through
			pCurOp += numTargets;
			goto JIT_SWITCH_end;
		}
		ofs = *(pCurOp + target);
		pCurOp = pOps + ofs;
	}
JIT_SWITCH_end:
	GO_NEXT_CHECK();

JIT_BRANCH_FALSE_PTR_start:
	OPCODE_USE(JIT_BRANCH_FALSE_PTR);
	{
		PTR value = POP_PTR();
		U32 ofs = GET_OP();
		if (value == NULL) {
			pCurOp = pOps + ofs;
		}
	}
JIT_BRANCH_FALSE_PTR_end:
	GO_NEXT_CHECK();

JIT_BRANCH_TRUE_PTR_start:
	OPCODE_USE(JIT_BRANCH_TRUE_PTR);
	{
		PTR value = POP_PTR();
		U32 ofs = GET_OP();
		if (value != NULL) {
			pCurOp = pOps + ofs;
		}
	}
JIT_BRANCH_TRUE_PTR_end:
	GO_NEXT_CHECK();

JIT_BRANCH_TRUE_start:
	OPCODE_USE(JIT_BRANCH_TRUE);
	{
		U32 value = POP_U32();
		U32 ofs = GET_OP();
		if (value != 0) {
			pCurOp = pOps + ofs;
		}
	}
JIT_BRANCH_TRUE_end:
	GO_NEXT_CHECK();

JIT_BRANCH_FALSE_start:
	OPCODE_USE(JIT_BRANCH_FALSE);
	{
		U32 value = POP_U32();
		U32 ofs = GET_OP();
		if (value == 0) {
			pCurOp = pOps + ofs;
		}
	}
JIT_BRANCH_FALSE_end:
	GO_NEXT_CHECK();

JIT_BEQ_I32I32_start:
	OPCODE_USE(JIT_BEQ_I32I32);
	{
		U32 v1, v2, ofs;
		POP_U32_U32(v1, v2);
		ofs = GET_OP();
		if ((I32)v1 == (I32)v2) {
			pCurOp = pOps + ofs;
		}
	}
JIT_BEQ_I32I32_end:
	GO_NEXT_CHECK();

JIT_BEQ_I64I64_start:
	OPCODE_USE(JIT_BEQ_I64I64);
	{
		U64 v1, v2;
		U32 ofs;
		POP_U64_U64(v1, v2);
		ofs = GET_OP();
		if ((I64)v1 == (I64)v2) {
			pCurOp = pOps + ofs;
		}
	}
JIT_BEQ_I64I64_end:
	GO_NEXT_CHECK();

JIT_BEQ_F32F32_start:
	OPCODE_USE(JIT_BEQ_F32F32);
	{
		float v1, v2;
		U32 ofs;
		POP_F32_F32(v1, v2);
		ofs = GET_OP();
		if (v1 == v2) {
			pCurOp = pOps + ofs;
		}
	}
JIT_BEQ_F32F32_end:
	GO_NEXT_CHECK();

JIT_BEQ_F64F64_start:
	OPCODE_USE(JIT_BEQ_F64F64);
	{
		double v1, v2;
		U32 ofs;
		POP_F64_F64(v1, v2);
		ofs = GET_OP();
		if (v1 == v2) {
			pCurOp = pOps + ofs;
		}
	}
JIT_BEQ_F64F64_end:
	GO_NEXT_CHECK();

JIT_BGE_I32I32_start:
	OPCODE_USE(JIT_BGE_I32I32);
	{
		U32 v1, v2, ofs;
		POP_U32_U32(v1, v2);
		ofs = GET_OP();
		if ((I32)v1 >= (I32)v2) {
			pCurOp = pOps + ofs;
		}
	}
JIT_BGE_I32I32_end:
	GO_NEXT_CHECK();

JIT_BGE_I64I64_start:
	OPCODE_USE(JIT_BGE_I64I64);
	{
		U64 v1, v2;
		U32 ofs;
		POP_U64_U64(v1, v2);
		ofs = GET_OP();
		if ((I64)v1 >= (I64)v2) {
			pCurOp = pOps + ofs;
		}
	}
JIT_BGE_I64I64_end:
	GO_NEXT_CHECK();

JIT_BGE_F32F32_start:
	OPCODE_USE(JIT_BGE_F32F32);
	{
		float v1, v2;
		U32 ofs;
		POP_F32_F32(v1, v2);
		ofs = GET_OP();
		if (v1 >= v2) {
			pCurOp = pOps + ofs;
		}
	}
JIT_BGE_F32F32_end:
	GO_NEXT_CHECK();

// the unordered form branches when either operand is NaN too (it used to be this handler, so NaN compared "true")
JIT_BGE_UN_F32F32_start:
	OPCODE_USE(JIT_BGE_UN_F32F32);
	{
		float v1, v2;
		U32 ofs;
		POP_F32_F32(v1, v2);
		ofs = GET_OP();
		if (!(v1 < v2)) {
			pCurOp = pOps + ofs;
		}
	}
JIT_BGE_UN_F32F32_end:
	GO_NEXT_CHECK();

JIT_BGE_F64F64_start:
	OPCODE_USE(JIT_BGE_F64F64);
	{
		double v1, v2;
		U32 ofs;
		POP_F64_F64(v1, v2);
		ofs = GET_OP();
		if (v1 >= v2) {
			pCurOp = pOps + ofs;
		}
	}
JIT_BGE_F64F64_end:
	GO_NEXT_CHECK();

// the unordered form branches when either operand is NaN too (it used to be this handler, so NaN compared "true")
JIT_BGE_UN_F64F64_start:
	OPCODE_USE(JIT_BGE_UN_F64F64);
	{
		double v1, v2;
		U32 ofs;
		POP_F64_F64(v1, v2);
		ofs = GET_OP();
		if (!(v1 < v2)) {
			pCurOp = pOps + ofs;
		}
	}
JIT_BGE_UN_F64F64_end:
	GO_NEXT_CHECK();

JIT_BGT_I32I32_start:
	OPCODE_USE(JIT_BGT_I32I32);
	{
		U32 v1, v2;
		U32 ofs;
		POP_U32_U32(v1, v2);
		ofs = GET_OP();
		if ((I32)v1 > (I32)v2) {
			pCurOp = pOps + ofs;
		}
	}
JIT_BGT_I32I32_end:
	GO_NEXT_CHECK();

JIT_BGT_I64I64_start:
	OPCODE_USE(JIT_BGT_I64I64);
	{
		U64 v1, v2;
		U32 ofs;
		POP_U64_U64(v1, v2);
		ofs = GET_OP();
		if ((I64)v1 > (I64)v2) {
			pCurOp = pOps + ofs;
		}
	}
JIT_BGT_I64I64_end:
	GO_NEXT_CHECK();

JIT_BGT_F32F32_start:
	OPCODE_USE(JIT_BGT_F32F32);
	{
		float v1, v2;
		U32 ofs;
		POP_F32_F32(v1, v2);
		ofs = GET_OP();
		if (v1 > v2) {
			pCurOp = pOps + ofs;
		}
	}
JIT_BGT_F32F32_end:
	GO_NEXT_CHECK();

// the unordered form branches when either operand is NaN too (it used to be this handler, so NaN compared "true")
JIT_BGT_UN_F32F32_start:
	OPCODE_USE(JIT_BGT_UN_F32F32);
	{
		float v1, v2;
		U32 ofs;
		POP_F32_F32(v1, v2);
		ofs = GET_OP();
		if (!(v1 <= v2)) {
			pCurOp = pOps + ofs;
		}
	}
JIT_BGT_UN_F32F32_end:
	GO_NEXT_CHECK();

JIT_BGT_F64F64_start:
	OPCODE_USE(JIT_BGT_F64F64);
	{
		double v1, v2;
		U32 ofs;
		POP_F64_F64(v1, v2);
		ofs = GET_OP();
		if (v1 > v2) {
			pCurOp = pOps + ofs;
		}
	}
JIT_BGT_F64F64_end:
	GO_NEXT_CHECK();

// the unordered form branches when either operand is NaN too (it used to be this handler, so NaN compared "true")
JIT_BGT_UN_F64F64_start:
	OPCODE_USE(JIT_BGT_UN_F64F64);
	{
		double v1, v2;
		U32 ofs;
		POP_F64_F64(v1, v2);
		ofs = GET_OP();
		if (!(v1 <= v2)) {
			pCurOp = pOps + ofs;
		}
	}
JIT_BGT_UN_F64F64_end:
	GO_NEXT_CHECK();

JIT_BLE_I32I32_start:
	OPCODE_USE(JIT_BLE_I32I32);
	{
		U32 v1, v2;
		U32 ofs;
		POP_U32_U32(v1, v2);
		ofs = GET_OP();
		if ((I32)v1 <= (I32)v2) {
			pCurOp = pOps + ofs;
		}
	}
JIT_BLE_I32I32_end:
	GO_NEXT_CHECK();

JIT_BLE_I64I64_start:
	OPCODE_USE(JIT_BLE_I64I64);
	{
		U64 v1, v2;
		U32 ofs;
		POP_U64_U64(v1, v2);
		ofs = GET_OP();
		if ((I64)v1 <= (I64)v2) {
			pCurOp = pOps + ofs;
		}
	}
JIT_BLE_I64I64_end:
	GO_NEXT_CHECK();

JIT_BLE_F32F32_start:
	OPCODE_USE(JIT_BLE_F32F32);
	{
		float v1, v2;
		U32 ofs;
		POP_F32_F32(v1, v2);
		ofs = GET_OP();
		if (v1 <= v2) {
			pCurOp = pOps + ofs;
		}
	}
JIT_BLE_F32F32_end:
	GO_NEXT_CHECK();

// the unordered form branches when either operand is NaN too (it used to be this handler, so NaN compared "true")
JIT_BLE_UN_F32F32_start:
	OPCODE_USE(JIT_BLE_UN_F32F32);
	{
		float v1, v2;
		U32 ofs;
		POP_F32_F32(v1, v2);
		ofs = GET_OP();
		if (!(v1 > v2)) {
			pCurOp = pOps + ofs;
		}
	}
JIT_BLE_UN_F32F32_end:
	GO_NEXT_CHECK();

JIT_BLE_F64F64_start:
	OPCODE_USE(JIT_BLE_F64F64);
	{
		double v1, v2;
		U32 ofs;
		POP_F64_F64(v1, v2);
		ofs = GET_OP();
		if (v1 <= v2) {
			pCurOp = pOps + ofs;
		}
	}
JIT_BLE_F64F64_end:
	GO_NEXT_CHECK();

// the unordered form branches when either operand is NaN too (it used to be this handler, so NaN compared "true")
JIT_BLE_UN_F64F64_start:
	OPCODE_USE(JIT_BLE_UN_F64F64);
	{
		double v1, v2;
		U32 ofs;
		POP_F64_F64(v1, v2);
		ofs = GET_OP();
		if (!(v1 > v2)) {
			pCurOp = pOps + ofs;
		}
	}
JIT_BLE_UN_F64F64_end:
	GO_NEXT_CHECK();

JIT_BLT_I32I32_start:
	OPCODE_USE(JIT_BLT_I32I32);
	{
		U32 v1, v2;
		U32 ofs;
		POP_U32_U32(v1, v2);
		ofs = GET_OP();
		if ((I32)v1 < (I32)v2) {
			pCurOp = pOps + ofs;
		}
	}
JIT_BLT_I32I32_end:
	GO_NEXT_CHECK();

JIT_BLT_I64I64_start:
	OPCODE_USE(JIT_BLT_I64I64);
	{
		U64 v1, v2;
		U32 ofs;
		POP_U64_U64(v1, v2);
		ofs = GET_OP();
		if ((I64)v1 < (I64)v2) {
			pCurOp = pOps + ofs;
		}
	}
JIT_BLT_I64I64_end:
	GO_NEXT_CHECK();

JIT_BLT_F32F32_start:
	OPCODE_USE(JIT_BLT_F32F32);
	{
		float v1, v2;
		U32 ofs;
		POP_F32_F32(v1, v2);
		ofs = GET_OP();
		if (v1 < v2) {
			pCurOp = pOps + ofs;
		}
	}
JIT_BLT_F32F32_end:
	GO_NEXT_CHECK();

// the unordered form branches when either operand is NaN too (it used to be this handler, so NaN compared "true")
JIT_BLT_UN_F32F32_start:
	OPCODE_USE(JIT_BLT_UN_F32F32);
	{
		float v1, v2;
		U32 ofs;
		POP_F32_F32(v1, v2);
		ofs = GET_OP();
		if (!(v1 >= v2)) {
			pCurOp = pOps + ofs;
		}
	}
JIT_BLT_UN_F32F32_end:
	GO_NEXT_CHECK();

JIT_BLT_F64F64_start:
	OPCODE_USE(JIT_BLT_F64F64);
	{
		double v1, v2;
		U32 ofs;
		POP_F64_F64(v1, v2);
		ofs = GET_OP();
		if (v1 < v2) {
			pCurOp = pOps + ofs;
		}
	}
JIT_BLT_F64F64_end:
	GO_NEXT_CHECK();

// the unordered form branches when either operand is NaN too (it used to be this handler, so NaN compared "true")
JIT_BLT_UN_F64F64_start:
	OPCODE_USE(JIT_BLT_UN_F64F64);
	{
		double v1, v2;
		U32 ofs;
		POP_F64_F64(v1, v2);
		ofs = GET_OP();
		if (!(v1 >= v2)) {
			pCurOp = pOps + ofs;
		}
	}
JIT_BLT_UN_F64F64_end:
	GO_NEXT_CHECK();

JIT_BNE_UN_I32I32_start:
	OPCODE_USE(JIT_BNE_UN_I32I32);
	{
		U32 v1, v2, ofs;
		POP_U32_U32(v1, v2);
		ofs = GET_OP();
		if (v1 != v2) {
			pCurOp = pOps + ofs;
		}
	}
JIT_BNE_UN_I32I32_end:
	GO_NEXT_CHECK();

JIT_BNE_UN_I64I64_start:
	OPCODE_USE(JIT_BNE_UN_I64I64);
	{
		U64 v1, v2;
		U32 ofs;
		POP_U64_U64(v1, v2);
		ofs = GET_OP();
		if (v1 != v2) {
			pCurOp = pOps + ofs;
		}
	}
JIT_BNE_UN_I64I64_end:
	GO_NEXT_CHECK();

JIT_BNE_UN_F32F32_start:
	OPCODE_USE(JIT_BNE_UN_F32F32);
	{
		float v1, v2;
		U32 ofs;
		POP_F32_F32(v1, v2);
		ofs = GET_OP();
		if (v1 != v2) {
			pCurOp = pOps + ofs;
		}
	}
JIT_BNE_UN_F32F32_end:
	GO_NEXT_CHECK();

JIT_BNE_UN_F64F64_start:
	OPCODE_USE(JIT_BNE_UN_F64F64);
	{
		double v1, v2;
		U32 ofs;
		POP_F64_F64(v1, v2);
		ofs = GET_OP();
		if (v1 != v2) {
			pCurOp = pOps + ofs;
		}
	}
JIT_BNE_UN_F64F64_end:
	GO_NEXT_CHECK();

JIT_BGE_UN_I32I32_start:
	OPCODE_USE(JIT_BGE_UN_I32I32);
	{
		U32 v1, v2, ofs;
		POP_U32_U32(v1, v2);
		ofs = GET_OP();
		if (v1 >= v2) {
			pCurOp = pOps + ofs;
		}
	}
JIT_BGE_UN_I32I32_end:
	GO_NEXT_CHECK();

JIT_BGT_UN_I32I32_start:
	OPCODE_USE(JIT_BGT_UN_I32I32);
	{
		U32 v1, v2, ofs;
		POP_U32_U32(v1, v2);
		ofs = GET_OP();
		if (v1 > v2) {
			pCurOp = pOps + ofs;
		}
	}
JIT_BGT_UN_I32I32_end:
	GO_NEXT_CHECK();

JIT_BLE_UN_I32I32_start:
	OPCODE_USE(JIT_BLE_UN_I32I32);
	{
		U32 v1, v2, ofs;
		POP_U32_U32(v1, v2);
		ofs = GET_OP();
		if (v1 <= v2) {
			pCurOp = pOps + ofs;
		}
	}
JIT_BLE_UN_I32I32_end:
	GO_NEXT_CHECK();

JIT_BLT_UN_I32I32_start:
	OPCODE_USE(JIT_BLT_UN_I32I32);
	{
		U32 v1, v2, ofs;
		POP_U32_U32(v1, v2);
		ofs = GET_OP();
		if (v1 < v2) {
			pCurOp = pOps + ofs;
		}
	}
JIT_BLT_UN_I32I32_end:
	GO_NEXT_CHECK();

JIT_BGE_UN_I64I64_start:
	OPCODE_USE(JIT_BGE_UN_I64I64);
	{
		U64 v1, v2;
		U32 ofs;
		POP_U64_U64(v1, v2);
		ofs = GET_OP();
		if (v1 >= v2) {
			pCurOp = pOps + ofs;
		}
	}
JIT_BGE_UN_I64I64_end:
	GO_NEXT_CHECK();

JIT_BGT_UN_I64I64_start:
	OPCODE_USE(JIT_BGT_UN_I64I64);
	{
		U64 v1, v2;
		U32 ofs;
		POP_U64_U64(v1, v2);
		ofs = GET_OP();
		if (v1 > v2) {
			pCurOp = pOps + ofs;
		}
	}
JIT_BGT_UN_I64I64_end:
	GO_NEXT_CHECK();

JIT_BLE_UN_I64I64_start:
	OPCODE_USE(JIT_BLE_UN_I64I64);
	{
		U64 v1, v2;
		U32 ofs;
		POP_U64_U64(v1, v2);
		ofs = GET_OP();
		if (v1 <= v2) {
			pCurOp = pOps + ofs;
		}
	}
JIT_BLE_UN_I64I64_end:
	GO_NEXT_CHECK();

JIT_BLT_UN_I64I64_start:
	OPCODE_USE(JIT_BLT_UN_I64I64);
	{
		U64 v1, v2;
		U32 ofs;
		POP_U64_U64(v1, v2);
		ofs = GET_OP();
		if (v1 < v2) {
			pCurOp = pOps + ofs;
		}
	}
JIT_BLT_UN_I64I64_end:
	GO_NEXT_CHECK();

JIT_CEQ_I32I32_start: // Handles I32 and O
	OPCODE_USE(JIT_CEQ_I32I32);
	BINARY_OP(U32, U32, U32, ==);
JIT_CEQ_I32I32_end:
	GO_NEXT();

JIT_CGT_I32I32_start:
	OPCODE_USE(JIT_CGT_I32I32);
	BINARY_OP(U32, I32, I32, >);
JIT_CGT_I32I32_end:
	GO_NEXT();

JIT_CGT_UN_I32I32_start: // Handles I32 and O
	OPCODE_USE(JIT_CGT_UN_I32I32);
	BINARY_OP(U32, U32, U32, >);
JIT_CGT_UN_I32I32_end:
	GO_NEXT();

JIT_CLT_I32I32_start:
	OPCODE_USE(JIT_CLT_I32I32);
	BINARY_OP(U32, I32, I32, <);
JIT_CLT_I32I32_end:
	GO_NEXT();

JIT_CLT_UN_I32I32_start:
	OPCODE_USE(JIT_CLT_UN_I32I32);
	BINARY_OP(U32, U32, U32, <);
JIT_CLT_UN_I32I32_end:
	GO_NEXT();

JIT_CEQ_I64I64_start:
	OPCODE_USE(JIT_CEQ_I64I64);
	BINARY_OP(U32, U64, U64, ==);
JIT_CEQ_I64I64_end:
	GO_NEXT();

JIT_CGT_I64I64_start:
	OPCODE_USE(JIT_CGT_I64I64);
	BINARY_OP(U32, I64, I64, >);
JIT_CGT_I64I64_end:
	GO_NEXT();

JIT_CGT_UN_I64I64_start:
	OPCODE_USE(JIT_CGT_UN_I64I64);
	BINARY_OP(U32, U64, U64, >);
JIT_CGT_UN_I64I64_end:
	GO_NEXT();

JIT_CLT_I64I64_start:
	OPCODE_USE(JIT_CLT_I64I64);
	BINARY_OP(U32, I64, I64, <);
JIT_CLT_I64I64_end:
	GO_NEXT();

JIT_CLT_UN_I64I64_start:
	OPCODE_USE(JIT_CLT_UN_I64I64);
	BINARY_OP(U32, U64, U64, <);
JIT_CLT_UN_I64I64_end:
	GO_NEXT();

JIT_CEQ_F32F32_start:
	OPCODE_USE(JIT_CEQ_F32F32);
	BINARY_OP(U32, float, float, ==);
JIT_CEQ_F32F32_end:
	GO_NEXT();

JIT_CEQ_F64F64_start:
	OPCODE_USE(JIT_CEQ_F64F64);
	BINARY_OP(U32, double, double, ==);
JIT_CEQ_F64F64_end:
	GO_NEXT();

// cgt.un / clt.un on floating point mean "greater / less than, OR UNORDERED" (either operand NaN),
// i.e. !(a <= b) and !(a >= b). C# uses them to build `a <= b` and `a >= b` as values
// (cgt.un; ldc.i4.0; ceq), which is how the opcodes are reached when not fused into a branch.
JIT_CGT_UN_F32F32_start:
	OPCODE_USE(JIT_CGT_UN_F32F32);
	{
		float v1, v2;
		POP_F32_F32(v1, v2);
		PUSH_U32(!(v1 <= v2));
	}
JIT_CGT_UN_F32F32_end:
	GO_NEXT();

JIT_CLT_UN_F32F32_start:
	OPCODE_USE(JIT_CLT_UN_F32F32);
	{
		float v1, v2;
		POP_F32_F32(v1, v2);
		PUSH_U32(!(v1 >= v2));
	}
JIT_CLT_UN_F32F32_end:
	GO_NEXT();

JIT_CGT_UN_F64F64_start:
	OPCODE_USE(JIT_CGT_UN_F64F64);
	{
		double v1, v2;
		POP_F64_F64(v1, v2);
		PUSH_U32(!(v1 <= v2));
	}
JIT_CGT_UN_F64F64_end:
	GO_NEXT();

JIT_CLT_UN_F64F64_start:
	OPCODE_USE(JIT_CLT_UN_F64F64);
	{
		double v1, v2;
		POP_F64_F64(v1, v2);
		PUSH_U32(!(v1 >= v2));
	}
JIT_CLT_UN_F64F64_end:
	GO_NEXT();

JIT_CGT_F32F32_start:
	OPCODE_USE(JIT_CGT_F32F32);
	BINARY_OP(U32, float, float, >);
JIT_CGT_F32F32_end:
	GO_NEXT();

JIT_CGT_F64F64_start:
	OPCODE_USE(JIT_CGT_F64F64);
	BINARY_OP(U32, double, double, >);
JIT_CGT_F64F64_end:
	GO_NEXT();

JIT_CLT_F32F32_start:
	OPCODE_USE(JIT_CLT_F32F32);
	BINARY_OP(U32, float, float, <);
JIT_CLT_F32F32_end:
	GO_NEXT();

JIT_CLT_F64F64_start:
	OPCODE_USE(JIT_CLT_F64F64);
	BINARY_OP(U32, double, double, <);
JIT_CLT_F64F64_end:
	GO_NEXT();

JIT_ADD_OVF_I32I32_start:
	OPCODE_USE(JIT_ADD_OVF_I32I32);
	{
		U32 v1, v2;
		I64 res;
		POP_U32_U32(v1, v2);
		res = (I64)(I32)v1 + (I64)(I32)v2;
		if (res > (I64)0x7fffffff || res < (I64)0xffffffff80000000) {
			// Overflowed, so throw exception
			THROW(types[TYPE_SYSTEM_OVERFLOWEXCEPTION]);
		}
		PUSH_U32((I32)res);
	}
JIT_ADD_OVF_I32I32_end:
	GO_NEXT();

JIT_ADD_OVF_UN_I32I32_start:
	OPCODE_USE(JIT_ADD_OVF_UN_I32I32);
	{
		U32 v1, v2;
		U64 res;
		POP_U32_U32(v1, v2);
		res = (U64)v1 + (U64)v2;
		if (res > (U64)0xffffffff) {
			// Overflowed, so throw exception
			THROW(types[TYPE_SYSTEM_OVERFLOWEXCEPTION]);
		}
		PUSH_U32(res);
	}
JIT_ADD_OVF_UN_I32I32_end:
	GO_NEXT();

JIT_MUL_OVF_I32I32_start:
	OPCODE_USE(JIT_MUL_OVF_I32I32);
	{
		U32 v1, v2;
		I64 res;
		POP_U32_U32(v1, v2);
		res = (I64)(I32)v1 * (I64)(I32)v2;
		if (res > (I64)0x7fffffff || res < (I64)0xffffffff80000000) {
			// Overflowed, so throw exception
			THROW(types[TYPE_SYSTEM_OVERFLOWEXCEPTION]);
		}
		PUSH_U32((I32)res);
	}
JIT_MUL_OVF_I32I32_end:
	GO_NEXT();

JIT_MUL_OVF_UN_I32I32_start:
	OPCODE_USE(JIT_MUL_OVF_UN_I32I32);
	{
		U32 v1, v2;
		U64 res;
		POP_U32_U32(v1, v2);
		res = (U64)v1 * (U64)v2;
		if (res > (U64)0xffffffff) {
			// Overflowed, so throw exception
			THROW(types[TYPE_SYSTEM_OVERFLOWEXCEPTION]);
		}
		PUSH_U32(res);
	}
JIT_MUL_OVF_UN_I32I32_end:
	GO_NEXT();

JIT_SUB_OVF_I32I32_start:
	OPCODE_USE(JIT_SUB_OVF_I32I32);
	{
		U32 v1, v2;
		I64 res;
		POP_U32_U32(v1, v2);
		res = (I64)(I32)v1 - (I64)(I32)v2;
		if (res > (I64)0x7fffffff || res < (I64)0xffffffff80000000) {
			// Overflowed, so throw exception
			THROW(types[TYPE_SYSTEM_OVERFLOWEXCEPTION]);
		}
		PUSH_U32((I32)res);
	}
JIT_SUB_OVF_I32I32_end:
	GO_NEXT();

JIT_SUB_OVF_UN_I32I32_start:
	OPCODE_USE(JIT_SUB_OVF_UN_I32I32);
	{
		U32 v1, v2;
		U64 res;
		POP_U32_U32(v1, v2);
		res = (U64)v1 - (U64)v2;
		if (res > (U64)0xffffffff) {
			// Overflowed, so throw exception
			THROW(types[TYPE_SYSTEM_OVERFLOWEXCEPTION]);
		}
		PUSH_U32(res);
	}
JIT_SUB_OVF_UN_I32I32_end:
	GO_NEXT();

JIT_ADD_I32I32_start:
	OPCODE_USE(JIT_ADD_I32I32);
	BINARY_OP(I32, I32, I32, +);
JIT_ADD_I32I32_end:
	GO_NEXT();

JIT_ADD_I64I64_start:
	OPCODE_USE(JIT_ADD_I64I64);
	BINARY_OP(I64, I64, I64, +);
JIT_ADD_I64I64_end:
	GO_NEXT();

JIT_ADD_F32F32_start:
	OPCODE_USE(JIT_ADD_F32F32);
	BINARY_OP(float, float, float, +);
JIT_ADD_F32F32_end:
	GO_NEXT();

JIT_ADD_F64F64_start:
	OPCODE_USE(JIT_ADD_F64F64);
	BINARY_OP(double, double, double, +);
JIT_ADD_F64F64_end:
	GO_NEXT();

JIT_SUB_I32I32_start:
	OPCODE_USE(JIT_SUB_I32I32);
	BINARY_OP(I32, I32, I32, -);
JIT_SUB_I32I32_end:
	GO_NEXT();

JIT_SUB_I64I64_start:
	OPCODE_USE(JIT_SUB_I64I64);
	BINARY_OP(I64, I64, I64, -);
JIT_SUB_I64I64_end:
	GO_NEXT();

JIT_SUB_F32F32_start:
	OPCODE_USE(JIT_SUB_F32F32);
	// Was BINARY_OP(double, double, double, -): it read two 8-byte doubles from a stack holding
	// two 4-byte floats, so every float subtraction was wrong and moved the stack by 4 bytes too much.
	BINARY_OP(float, float, float, -);
JIT_SUB_F32F32_end:
	GO_NEXT();

JIT_SUB_F64F64_start:
	OPCODE_USE(JIT_SUB_F64F64);
	BINARY_OP(double, double, double, -);
JIT_SUB_F64F64_end:
	GO_NEXT();

JIT_MUL_I32I32_start:
	OPCODE_USE(JIT_MUL_I32I32);
	BINARY_OP(I32, I32, I32, *);
JIT_MUL_I32I32_end:
	GO_NEXT();

JIT_MUL_I64I64_start:
	OPCODE_USE(JIT_MUL_I64I64);
	BINARY_OP(I64, I64, I64, *);
JIT_MUL_I64I64_end:
	GO_NEXT();

JIT_MUL_F32F32_start:
	OPCODE_USE(JIT_MUL_F32F32);
	BINARY_OP(float, float, float, *);
JIT_MUL_F32F32_end:
	GO_NEXT();

JIT_MUL_F64F64_start:
	OPCODE_USE(JIT_MUL_F64F64);
	BINARY_OP(double, double, double, *);
JIT_MUL_F64F64_end:
	GO_NEXT();

JIT_DIV_I32I32_start:
	OPCODE_USE(JIT_DIV_I32I32);
	{
		I32 v1, v2;
		POP_U32_U32(v1, v2);
		if (v2 == 0) {
			THROW(types[TYPE_SYSTEM_DIVIDEBYZEROEXCEPTION]);
		}
		if (v2 == -1 && v1 == (I32)0x80000000) {
			// MinValue / -1 (or % -1) overflows
			THROW(types[TYPE_SYSTEM_OVERFLOWEXCEPTION]);
		}
		PUSH_U32(v1 / v2);
	}
JIT_DIV_I32I32_end:
	GO_NEXT();

JIT_DIV_I64I64_start:
	OPCODE_USE(JIT_DIV_I64I64);
	{
		I64 v1, v2;
		POP_U64_U64(v1, v2);
		if (v2 == 0) {
			THROW(types[TYPE_SYSTEM_DIVIDEBYZEROEXCEPTION]);
		}
		if (v2 == -1 && v1 == (I64)0x8000000000000000ULL) {
			// MinValue / -1 (or % -1) overflows
			THROW(types[TYPE_SYSTEM_OVERFLOWEXCEPTION]);
		}
		PUSH_U64(v1 / v2);
	}
JIT_DIV_I64I64_end:
	GO_NEXT();

JIT_DIV_F32F32_start:
	OPCODE_USE(JIT_DIV_F32F32);
	BINARY_OP(float, float, float, /);
JIT_DIV_F32F32_end:
	GO_NEXT();

JIT_DIV_F64F64_start:
	OPCODE_USE(JIT_DIV_F64F64);
	BINARY_OP(double, double, double, /);
JIT_DIV_F64F64_end:
	GO_NEXT();

JIT_DIV_UN_I32I32_start:
	OPCODE_USE(JIT_DIV_UN_I32I32);
	{
		U32 v1, v2;
		POP_U32_U32(v1, v2);
		if (v2 == 0) {
			THROW(types[TYPE_SYSTEM_DIVIDEBYZEROEXCEPTION]);
		}
		PUSH_U32(v1 / v2);
	}
JIT_DIV_UN_I32I32_end:
	GO_NEXT();

JIT_DIV_UN_I64I64_start:
	OPCODE_USE(JIT_DIV_UN_I64I64);
	{
		U64 v1, v2;
		POP_U64_U64(v1, v2);
		if (v2 == 0) {
			THROW(types[TYPE_SYSTEM_DIVIDEBYZEROEXCEPTION]);
		}
		PUSH_U64(v1 / v2);
	}
JIT_DIV_UN_I64I64_end:
	GO_NEXT();

JIT_REM_I32I32_start:
	OPCODE_USE(JIT_REM_I32I32);
	{
		I32 v1, v2;
		POP_U32_U32(v1, v2);
		if (v2 == 0) {
			THROW(types[TYPE_SYSTEM_DIVIDEBYZEROEXCEPTION]);
		}
		if (v2 == -1 && v1 == (I32)0x80000000) {
			// MinValue / -1 (or % -1) overflows
			THROW(types[TYPE_SYSTEM_OVERFLOWEXCEPTION]);
		}
		PUSH_U32(v1 % v2);
	}
JIT_REM_I32I32_end:
	GO_NEXT();

JIT_REM_I64I64_start:
	OPCODE_USE(JIT_REM_I64I64);
	{
		I64 v1, v2;
		POP_U64_U64(v1, v2);
		if (v2 == 0) {
			THROW(types[TYPE_SYSTEM_DIVIDEBYZEROEXCEPTION]);
		}
		if (v2 == -1 && v1 == (I64)0x8000000000000000ULL) {
			// MinValue / -1 (or % -1) overflows
			THROW(types[TYPE_SYSTEM_OVERFLOWEXCEPTION]);
		}
		PUSH_U64(v1 % v2);
	}
JIT_REM_I64I64_end:
	GO_NEXT();

JIT_ADD_OVF_I64I64_start:
	OPCODE_USE(JIT_ADD_OVF_I64I64);
	{
		U64 v1, v2;
		I64 res;
		POP_U64_U64(v1, v2);
		if (AddOvfI64((I64)v1, (I64)v2, &res)) {
			// Overflowed, so throw exception
			THROW(types[TYPE_SYSTEM_OVERFLOWEXCEPTION]);
		}
		PUSH_U64((U64)res);
	}
JIT_ADD_OVF_I64I64_end:
	GO_NEXT();

JIT_ADD_OVF_UN_I64I64_start:
	OPCODE_USE(JIT_ADD_OVF_UN_I64I64);
	{
		U64 v1, v2;
		U64 res;
		POP_U64_U64(v1, v2);
		if (AddOvfU64((U64)v1, (U64)v2, &res)) {
			// Overflowed, so throw exception
			THROW(types[TYPE_SYSTEM_OVERFLOWEXCEPTION]);
		}
		PUSH_U64((U64)res);
	}
JIT_ADD_OVF_UN_I64I64_end:
	GO_NEXT();

JIT_SUB_OVF_I64I64_start:
	OPCODE_USE(JIT_SUB_OVF_I64I64);
	{
		U64 v1, v2;
		I64 res;
		POP_U64_U64(v1, v2);
		if (SubOvfI64((I64)v1, (I64)v2, &res)) {
			// Overflowed, so throw exception
			THROW(types[TYPE_SYSTEM_OVERFLOWEXCEPTION]);
		}
		PUSH_U64((U64)res);
	}
JIT_SUB_OVF_I64I64_end:
	GO_NEXT();

JIT_SUB_OVF_UN_I64I64_start:
	OPCODE_USE(JIT_SUB_OVF_UN_I64I64);
	{
		U64 v1, v2;
		U64 res;
		POP_U64_U64(v1, v2);
		if (SubOvfU64((U64)v1, (U64)v2, &res)) {
			// Overflowed, so throw exception
			THROW(types[TYPE_SYSTEM_OVERFLOWEXCEPTION]);
		}
		PUSH_U64((U64)res);
	}
JIT_SUB_OVF_UN_I64I64_end:
	GO_NEXT();

JIT_MUL_OVF_I64I64_start:
	OPCODE_USE(JIT_MUL_OVF_I64I64);
	{
		U64 v1, v2;
		I64 res;
		POP_U64_U64(v1, v2);
		if (MulOvfI64((I64)v1, (I64)v2, &res)) {
			// Overflowed, so throw exception
			THROW(types[TYPE_SYSTEM_OVERFLOWEXCEPTION]);
		}
		PUSH_U64((U64)res);
	}
JIT_MUL_OVF_I64I64_end:
	GO_NEXT();

JIT_MUL_OVF_UN_I64I64_start:
	OPCODE_USE(JIT_MUL_OVF_UN_I64I64);
	{
		U64 v1, v2;
		U64 res;
		POP_U64_U64(v1, v2);
		if (MulOvfU64((U64)v1, (U64)v2, &res)) {
			// Overflowed, so throw exception
			THROW(types[TYPE_SYSTEM_OVERFLOWEXCEPTION]);
		}
		PUSH_U64((U64)res);
	}
JIT_MUL_OVF_UN_I64I64_end:
	GO_NEXT();

JIT_REM_F32F32_start:
	OPCODE_USE(JIT_REM_F32F32);
	{
		float v2 = STACK_ADDR(float);
		pCurEvalStack -= sizeof(float);
		STACK_ADDR(float) = fmodf(STACK_ADDR(float), v2);
	}
JIT_REM_F32F32_end:
	GO_NEXT();

JIT_REM_F64F64_start:
	OPCODE_USE(JIT_REM_F64F64);
	{
		double v2 = STACK_ADDR(double);
		pCurEvalStack -= sizeof(double);
		STACK_ADDR(double) = fmod(STACK_ADDR(double), v2);
	}
JIT_REM_F64F64_end:
	GO_NEXT();

JIT_CONV_OVF_CHECK_start:
	OPCODE_USE(JIT_CONV_OVF_CHECK);
	{
		U32 param = GET_OP();
		if (!ConvOvfFits(param & 0xff, param >> 8, pCurEvalStack)) {
			// Does not fit the destination type, so throw exception
			THROW(types[TYPE_SYSTEM_OVERFLOWEXCEPTION]);
		}
	}
JIT_CONV_OVF_CHECK_end:
	GO_NEXT();

JIT_REM_UN_I32I32_start:
	OPCODE_USE(JIT_REM_UN_I32I32);
	{
		U32 v1, v2;
		POP_U32_U32(v1, v2);
		if (v2 == 0) {
			THROW(types[TYPE_SYSTEM_DIVIDEBYZEROEXCEPTION]);
		}
		PUSH_U32(v1 % v2);
	}
JIT_REM_UN_I32I32_end:
	GO_NEXT();

JIT_REM_UN_I64I64_start:
	OPCODE_USE(JIT_REM_UN_I64I64);
	{
		U64 v1, v2;
		POP_U64_U64(v1, v2);
		if (v2 == 0) {
			THROW(types[TYPE_SYSTEM_DIVIDEBYZEROEXCEPTION]);
		}
		PUSH_U64(v1 % v2);
	}
JIT_REM_UN_I64I64_end:
	GO_NEXT();

JIT_AND_I32I32_start:
	OPCODE_USE(JIT_AND_I32I32);
	BINARY_OP(U32, U32, U32, &);
JIT_AND_I32I32_end:
	GO_NEXT();

JIT_AND_I64I64_start:
	OPCODE_USE(JIT_AND_I64I64);
	BINARY_OP(U64, U64, U64, &);
JIT_AND_I64I64_end:
	GO_NEXT();

JIT_OR_I32I32_start:
	OPCODE_USE(JIT_OR_I32I32);
	BINARY_OP(U32, U32, U32, |);
JIT_OR_I32I32_end:
	GO_NEXT();

JIT_OR_I64I64_start:
	OPCODE_USE(JIT_OR_I64I64);
	BINARY_OP(U64, U64, U64, |);
JIT_OR_I64I64_end:
	GO_NEXT();

JIT_XOR_I32I32_start:
	OPCODE_USE(JIT_XOR_I32I32);
	BINARY_OP(U32, U32, U32, ^);
JIT_XOR_I32I32_end:
	GO_NEXT();

JIT_XOR_I64I64_start:
	OPCODE_USE(JIT_XOR_I64I64);
	BINARY_OP(U64, U64, U64, ^);
JIT_XOR_I64I64_end:
	GO_NEXT();

JIT_NEG_I32_start:
	OPCODE_USE(JIT_NEG_I32);
	UNARY_OP(I32, -);
JIT_NEG_I32_end:
	GO_NEXT();

JIT_NEG_I64_start:
	OPCODE_USE(JIT_NEG_I64);
	UNARY_OP(I64, -);
JIT_NEG_I64_end:
	GO_NEXT();

JIT_NEG_F32_start:
	OPCODE_USE(JIT_NEG_F32);
	UNARY_OP(float, -);
JIT_NEG_F32_end:
	GO_NEXT();

JIT_NEG_F64_start:
	OPCODE_USE(JIT_NEG_F64);
	UNARY_OP(double, -);
JIT_NEG_F64_end:
	GO_NEXT();

JIT_COPYOBJECT_start:
	OPCODE_USE(JIT_COPYOBJECT);
	{
		tMD_TypeDef *pCpType = (tMD_TypeDef*)GET_OP();
		PTR pSrc = POP_PTR();
		PTR pDst = POP_PTR();
		if (pSrc == NULL || pDst == NULL) {
			THROW_NULLREF();
		}
		CopyObject(pCpType, pDst, pSrc);
	}
JIT_COPYOBJECT_end:
	GO_NEXT();

JIT_CKFINITE_F32_start:
	OPCODE_USE(JIT_CKFINITE_F32);
	if (!isfinite(STACK_ADDR(float))) {
		// NaN or infinity. The reference runtimes throw OverflowException here.
		THROW(types[TYPE_SYSTEM_OVERFLOWEXCEPTION]);
	}
JIT_CKFINITE_F32_end:
	GO_NEXT();

JIT_CKFINITE_F64_start:
	OPCODE_USE(JIT_CKFINITE_F64);
	if (!isfinite(STACK_ADDR(double))) {
		THROW(types[TYPE_SYSTEM_OVERFLOWEXCEPTION]);
	}
JIT_CKFINITE_F64_end:
	GO_NEXT();

JIT_NOT_I32_start:
	OPCODE_USE(JIT_NOT_I32);
	UNARY_OP(U32, ~);
JIT_NOT_I32_end:
	GO_NEXT();

JIT_NOT_I64_start:
	OPCODE_USE(JIT_NOT_I64);
	UNARY_OP(U64, ~);
JIT_NOT_I64_end:
	GO_NEXT();

JIT_SHL_I32_start:
	OPCODE_USE(JIT_SHL_I32);
	BINARY_OP(U32, U32, U32, <<);
JIT_SHL_I32_end:
	GO_NEXT();

JIT_SHR_I32_start:
	OPCODE_USE(JIT_SHR_I32);
	BINARY_OP(I32, I32, U32, >>);
JIT_SHR_I32_end:
	GO_NEXT();

JIT_SHR_UN_I32_start:
	OPCODE_USE(JIT_SHR_UN_I32);
	BINARY_OP(U32, U32, U32, >>);
JIT_SHR_UN_I32_end:
	GO_NEXT();

JIT_SHL_I64_start:
	OPCODE_USE(JIT_SHL_I64);
	BINARY_OP(U64, U64, U32, <<);
JIT_SHL_I64_end:
	GO_NEXT();

JIT_SHR_I64_start:
	OPCODE_USE(JIT_SHR_I64);
	BINARY_OP(I64, I64, U32, >>);
JIT_SHR_I64_end:
	GO_NEXT();

JIT_SHR_UN_I64_start:
	OPCODE_USE(JIT_SHR_UN_I64);
	BINARY_OP(U64, U64, U32, >>);
JIT_SHR_UN_I64_end:
	GO_NEXT();

	// Conversion operations

JIT_CONV_U32_U32_start:
JIT_CONV_I32_U32_start:
	OPCODE_USE(JIT_CONV_I32_U32);
	{
		U32 mask = GET_OP();
		STACK_ADDR(U32) &= mask;
	}
JIT_CONV_U32_U32_end:
JIT_CONV_I32_U32_end:
	GO_NEXT();

JIT_CONV_U32_I32_start:
JIT_CONV_I32_I32_start:
	OPCODE_USE(JIT_CONV_I32_I32);
	{
		U32 shift = GET_OP();
		STACK_ADDR(I32) = (STACK_ADDR(I32) << shift) >> shift;
	}
JIT_CONV_U32_I32_end:
JIT_CONV_I32_I32_end:
	GO_NEXT();

JIT_CONV_I32_I64_start:
	OPCODE_USE(JIT_CONV_I32_I64);
	{
		I32 value = (I32)POP_U32();
		PUSH_U64((I64)value);
	}
JIT_CONV_I32_I64_end:
	GO_NEXT();

JIT_CONV_I32_U64_start:
JIT_CONV_U32_U64_start:
JIT_CONV_U32_I64_start:
	OPCODE_USE(JIT_CONV_U32_I64);
	{
		U32 value = POP_U32();
		PUSH_U64(value);
	}
JIT_CONV_I32_U64_end:
JIT_CONV_U32_U64_end:
JIT_CONV_U32_I64_end:
	GO_NEXT();

JIT_CONV_I32_R32_start:
	OPCODE_USE(JIT_CONV_I32_R32);
	{
		I32 value = (I32)POP_U32();
		PUSH_FLOAT(value);
	}
JIT_CONV_I32_R32_end:
	GO_NEXT();

JIT_CONV_I32_R64_start:
	OPCODE_USE(JIT_CONV_I32_R64);
	{
		I32 value = (I32)POP_U32();
		PUSH_DOUBLE(value);
	}
JIT_CONV_I32_R64_end:
	GO_NEXT();

JIT_CONV_U32_R32_start:
	OPCODE_USE(JIT_CONV_U32_R32);
	{
		U32 value = POP_U32();
		PUSH_FLOAT(value);
	}
JIT_CONV_U32_R32_end:
	GO_NEXT();

JIT_CONV_U32_R64_start:
	OPCODE_USE(JIT_CONV_U32_R64);
	{
		U32 value = POP_U32();
		PUSH_DOUBLE(value);
	}
JIT_CONV_U32_R64_end:
	GO_NEXT();

JIT_CONV_I64_U32_start:
JIT_CONV_U64_U32_start:
	OPCODE_USE(JIT_CONV_I64_U32);
	{
		U32 mask = GET_OP();
		U64 value = POP_U64();
		PUSH_U32(value & mask);
	}
JIT_CONV_I64_U32_end:
JIT_CONV_U64_U32_end:
	GO_NEXT();

JIT_CONV_I64_I32_start:
JIT_CONV_U64_I32_start:
	OPCODE_USE(JIT_CONV_I64_U32);
	{
		U32 shift = GET_OP();
		I32 value = (I32)POP_U64();
		value = (value << shift) >> shift;
		PUSH_U32(value);
	}
JIT_CONV_I64_I32_end:
JIT_CONV_U64_I32_end:
	GO_NEXT();

JIT_CONV_I64_R32_start:
	OPCODE_USE(JIT_CONV_I64_R32);
	{
		I64 value = (I64)POP_U64();
		PUSH_FLOAT(value);
	}
JIT_CONV_I64_R32_end:
	GO_NEXT();

JIT_CONV_I64_R64_start:
	OPCODE_USE(JIT_CONV_I64_R64);
	{
		I64 value = (I64)POP_U64();
		PUSH_DOUBLE(value);
	}
JIT_CONV_I64_R64_end:
	GO_NEXT();

JIT_CONV_U64_R32_start:
	OPCODE_USE(JIT_CONV_U64_R32);
	{
		U64 value = POP_U64();
		PUSH_FLOAT(value);
	}
JIT_CONV_U64_R32_end:
	GO_NEXT();

JIT_CONV_U64_R64_start:
	OPCODE_USE(JIT_CONV_U64_R64);
	{
		U64 value = POP_U64();
		PUSH_DOUBLE(value);
	}
JIT_CONV_U64_R64_end:
	GO_NEXT();

JIT_CONV_R32_I32_start:
	OPCODE_USE(JIT_CONV_R32_I32);
	{
		U32 shift = GET_OP();
		I32 result;
		float value = POP_FLOAT();
		result = (I32)value;
		result = (result << shift) >> shift;
		PUSH_U32(result);
	}
JIT_CONV_R32_I32_end:
	GO_NEXT();

JIT_CONV_R32_U32_start:
	OPCODE_USE(JIT_CONV_R32_U32);
	{
		U32 mask = GET_OP();
		float value = POP_FLOAT();
		PUSH_U32(((U32)value) & mask);
	}
JIT_CONV_R32_U32_end:
	GO_NEXT();

JIT_CONV_R32_I64_start:
	OPCODE_USE(JIT_CONV_R32_I64);
	{
		float value = POP_FLOAT();
		PUSH_U64((I64)value);
	}
JIT_CONV_R32_I64_end:
	GO_NEXT();

JIT_CONV_R32_U64_start:
	OPCODE_USE(JIT_CONV_R32_U64);
	{
		float value = POP_FLOAT();
		PUSH_U64(value);
	}
JIT_CONV_R32_U64_end:
	GO_NEXT();

JIT_CONV_R32_R64_start:
	OPCODE_USE(JIT_CONV_R32_R64);
	{
		float value = POP_FLOAT();
		PUSH_DOUBLE(value);
	}
JIT_CONV_R32_R64_end:
	GO_NEXT();

JIT_CONV_R64_I32_start:
	OPCODE_USE(JIT_CONV_R64_I32);
	{
		U32 shift = GET_OP();
		I32 result;
		double value = POP_DOUBLE();
		result = (I32)value;
		result = (result << shift) >> shift;
		PUSH_U32(result);
	}
JIT_CONV_R64_I32_end:
	GO_NEXT();

JIT_CONV_R64_U32_start:
	OPCODE_USE(JIT_CONV_R64_U32);
	{
		U32 mask = GET_OP();
		double value = POP_DOUBLE();
		PUSH_U32(((U32)value) & mask);
	}
JIT_CONV_R64_U32_end:
	GO_NEXT();

JIT_CONV_R64_I64_start:
	OPCODE_USE(JIT_CONV_R64_I64);
	{
		// Was `float value = POP_FLOAT();` (copied from the R32 version): it popped 4 bytes of an
		// 8-byte double, converted the wrong half, and left the stack 4 bytes out.
		double value = POP_DOUBLE();
		PUSH_U64((I64)value);
	}
JIT_CONV_R64_I64_end:
	GO_NEXT();

JIT_CONV_R64_U64_start:
	OPCODE_USE(JIT_CONV_R64_U64);
	{
		double value = POP_DOUBLE();
		PUSH_U64(value);
	}
JIT_CONV_R64_U64_end:
	GO_NEXT();

JIT_CONV_R64_R32_start:
	OPCODE_USE(JIT_CONV_R64_R32);
	{
		float value = (float)POP_DOUBLE();
		PUSH_FLOAT(value);
	}
JIT_CONV_R64_R32_end:
	GO_NEXT();

JIT_LOADFUNCTION_start:
	OPCODE_USE(JIT_LOADFUNCTION);
	{
		// A pointer to the method (this used to be pushed as a U32, which truncated it)
		PTR value = (PTR)GET_OP();
		PUSH_PTR(value);
	}
JIT_LOADFUNCTION_end:
	GO_NEXT();

JIT_LOADVIRTFUNCTION_start:
	OPCODE_USE(JIT_LOADVIRTFUNCTION);
	{
		tMD_MethodDef *pVirtMethod = (tMD_MethodDef*)GET_OP();
		HEAP_PTR pObj = POP_O();
		tMD_MethodDef *pResolved;
		if (pObj == NULL) {
			THROW_NULLREF();
		}
		pResolved = ResolveVirtualMethod(pVirtMethod, Heap_GetType(pObj));
		if (pResolved == NULL) {
			THROW(types[TYPE_SYSTEM_INVALIDCASTEXCEPTION]);
		}
		PUSH_PTR(pResolved);
	}
JIT_LOADVIRTFUNCTION_end:
	GO_NEXT();

// A TypedReference is two pointers on the stack: the address, then the type.
JIT_MKREFANY_start:
	OPCODE_USE(JIT_MKREFANY);
	{
		tMD_TypeDef *pRefType = (tMD_TypeDef*)GET_OP();
		PTR pAddr = POP_PTR();
		PUSH_PTR(pAddr);
		PUSH_PTR((PTR)pRefType);
	}
JIT_MKREFANY_end:
	GO_NEXT();

JIT_REFANYVAL_start:
	OPCODE_USE(JIT_REFANYVAL);
	{
		tMD_TypeDef *pWanted = (tMD_TypeDef*)GET_OP();
		tMD_TypeDef *pHeld = (tMD_TypeDef*)POP_PTR();
		PTR pAddr = POP_PTR();
		if (pHeld != pWanted) {
			// the reference was made for a different type
			THROW(types[TYPE_SYSTEM_INVALIDCASTEXCEPTION]);
		}
		PUSH_PTR(pAddr);
	}
JIT_REFANYVAL_end:
	GO_NEXT();

JIT_REFANYTYPE_start:
	OPCODE_USE(JIT_REFANYTYPE);
	{
		PTR pType = POP_PTR();
		(void)POP_PTR();
		PUSH_PTR(pType);
	}
JIT_REFANYTYPE_end:
	GO_NEXT();

JIT_JMP_COPYARGS_start:
	OPCODE_USE(JIT_JMP_COPYARGS);
	{
		// Arguments sit in the frame laid out exactly as they sit on the evaluation stack
		U32 size = pCurrentMethodState->pMethod->parameterStackSize;
		memcpy(pCurEvalStack, pParamsLocals, size);
		pCurEvalStack += size;
	}
JIT_JMP_COPYARGS_end:
	GO_NEXT();

JIT_CPBLK_start:
	OPCODE_USE(JIT_CPBLK);
	{
		U32 count = POP_U32();
		PTR pSrc = POP_PTR();
		PTR pDst = POP_PTR();
		if (count != 0) {
			if (pSrc == NULL || pDst == NULL) {
				THROW_NULLREF();
			}
			memmove(pDst, pSrc, count);
		}
	}
JIT_CPBLK_end:
	GO_NEXT();

JIT_INITBLK_start:
	OPCODE_USE(JIT_INITBLK);
	{
		U32 count = POP_U32();
		U32 value = POP_U32();
		PTR pDst = POP_PTR();
		if (count != 0) {
			if (pDst == NULL) {
				THROW_NULLREF();
			}
			memset(pDst, (int)(value & 0xff), count);
		}
	}
JIT_INITBLK_end:
	GO_NEXT();

JIT_LOADOBJECT_start:
	OPCODE_USE(JIT_LOADOBJECT);
	{
		tMD_TypeDef *pTypeDef;
		PTR pMem;

		pMem = POP_PTR(); // address of value-type
		pTypeDef = (tMD_TypeDef*)GET_OP(); //type of the value-type
		//if (pTypeDef->stackSize != pTypeDef->arrayElementSize) {
			// For bytes and int16s we need some special code to ensure that the stack
			// does not contain rubbish in the bits unused in this type.
			// But there is no harm in running this for all types, and it's smaller and probably faster
			*(U32*)pCurEvalStack = 0;
		//}
		PUSH_VALUETYPE(pMem, pTypeDef->arrayElementSize, pTypeDef->stackSize);
	}
JIT_LOADOBJECT_end:
	GO_NEXT();

JIT_LOAD_STRING_start:
	OPCODE_USE(JIT_LOAD_STRING);
	{
		U32 value = GET_OP();
		PTR heapPtr = SystemString_FromUserStrings(pCurrentMethodState->pMetaData, value);
		PUSH_O(heapPtr);
	}
JIT_LOAD_STRING_end:
	GO_NEXT();

JIT_FFI_CALL_start:
	OPCODE_USE(JIT_FFI_CALL);
	{
		// The arguments are on the evaluation stack in the order of the call; the wrapper reads them there, calls the C function and writes
		// the result where the first argument was. No frame, no copy, no marshalling.
		const tFFIEntry *pFfi = (const tFFIEntry*)GET_OP();
		pFfi->wrapper(pCurEvalStack - pFfi->argBytes);
		pCurEvalStack = pCurEvalStack - pFfi->argBytes + pFfi->retBytes;
	}
JIT_FFI_CALL_end:
	GO_NEXT();

JIT_LOAD_STRING_MD_start:
	OPCODE_USE(JIT_LOAD_STRING_MD);
	{
		// ldstr from a method that was inlined into another: the token belongs to the metadata of the method it came from, not the one running
		tMetaData *pStringMetaData = (tMetaData*)GET_OP();
		U32 value = GET_OP();
		PTR heapPtr = SystemString_FromUserStrings(pStringMetaData, value);
		PUSH_O(heapPtr);
	}
JIT_LOAD_STRING_MD_end:
	GO_NEXT();

JIT_NEWOBJECT_start:
	OPCODE_USE(JIT_NEWOBJECT);
	{
		tMD_MethodDef *pConstructorDef;
		HEAP_PTR obj;
		tMethodState *pCallMethodState;
		U32 isInternalConstructor;
		PTR pTempPtr;

		pConstructorDef = (tMD_MethodDef*)GET_OP();
		isInternalConstructor = (pConstructorDef->implFlags & METHODIMPLATTRIBUTES_INTERNALCALL) != 0;

		if (!isInternalConstructor) {
			// All internal constructors MUST allocate their own 'this' objects
			obj = Heap_AllocType(pConstructorDef->pParentType);
		} else {
			// Need to set this to something non-NULL so that CreateParameters() works properly
			obj = (HEAP_PTR)-1;
		}

		// Set up the new method state for the called method
		pCallMethodState = MethodState_Direct(pThread, pConstructorDef, pCurrentMethodState, isInternalConstructor);
		// Fill in the parameters
		pTempPtr = pCurEvalStack;
		CreateParameters(pCallMethodState->pParamsLocals, pConstructorDef, &pTempPtr, obj);
		pCurEvalStack = pTempPtr;
		if (!isInternalConstructor) {
			// Push the object here, so it's on the stack when the constructor returns
			PUSH_O(obj);
		}
		// Set up the local variables for the new method state (for the obj constructor)
		CHANGE_METHOD_STATE(pCallMethodState);
		// Run any pending Finalizers
		RUN_FINALIZER();
	}
JIT_NEWOBJECT_end:
	GO_NEXT_CHECK();

JIT_NEWOBJECT_VALUETYPE_start:
	OPCODE_USE(JIT_NEWOBJECT_VALUETYPE);
	{
		tMD_MethodDef *pConstructorDef;
		tMethodState *pCallMethodState;
		U32 isInternalConstructor;
		PTR pTempPtr, pMem;

		pConstructorDef = (tMD_MethodDef*)GET_OP();
		isInternalConstructor = (pConstructorDef->implFlags & METHODIMPLATTRIBUTES_INTERNALCALL) != 0;

		// Allocate space on the eval-stack for the new value-type here
		pMem = pCurEvalStack - (pConstructorDef->parameterStackSize - sizeof(PTR));

		// Set up the new method state for the called method
		pCallMethodState = MethodState_Direct(pThread, pConstructorDef, pCurrentMethodState, isInternalConstructor);
		// Fill in the parameters
		pTempPtr = pCurEvalStack;
		CreateParameters(pCallMethodState->pParamsLocals, pConstructorDef, &pTempPtr, pMem);
		pCurEvalStack = pTempPtr;
		// Set the stack state so it's correct for the constructor return
		pCurEvalStack += pConstructorDef->pParentType->stackSize;
		// Set up the local variables for the new method state
		CHANGE_METHOD_STATE(pCallMethodState);
		// Run any pending Finalizers
		RUN_FINALIZER();
	}
JIT_NEWOBJECT_VALUETYPE_end:
	GO_NEXT_CHECK();

JIT_IS_INSTANCE_start:
	op = JIT_IS_INSTANCE;
	goto jitCastClass;
JIT_CAST_CLASS_start:
	op = JIT_CAST_CLASS;
jitCastClass:
	OPCODE_USE(JIT_CAST_CLASS);
	{
		tMD_TypeDef *pToType, *pTestType;
		HEAP_PTR heapPtr;

		pToType = (tMD_TypeDef*)GET_OP();
		heapPtr = POP_O();
		if (heapPtr == NULL) {
			PUSH_O(NULL);
			goto JIT_IS_INSTANCE_end;
		}
		pTestType = Heap_GetType(heapPtr);
		if (pTestType == pToType) {
			// exactly that type: nothing to work out
			PUSH_O(heapPtr);
			goto JIT_IS_INSTANCE_end;
		}
		if (TYPE_ISINTERFACE(pToType)) {
			// an interface that the object's type lists itself (the usual case) is found without the general walk
			U32 ii;
			for (ii = 0; ii < pTestType->numInterfaces; ii++) {
				if (pTestType->pInterfaceMaps[ii].pInterface == pToType) {
					PUSH_O(heapPtr);
					goto JIT_IS_INSTANCE_end;
				}
			}
		}
		if (TYPE_ISARRAY(pTestType) && TYPE_ISARRAY(pToType)) {
			// Arrays are handled specially - check if the element type is compatible
			tMD_TypeDef *pToElem = pToType->pArrayElementType, *pTestElem = pTestType->pArrayElementType;
			MetaData_Fill_TypeDef(pTestElem, NULL, NULL);
			// array covariance is for reference element types only: an int[] is not an object[] (it used to be one)
			if (pToElem == pTestElem || (!pTestElem->isValueType && Type_IsAssignableFrom(pToElem, pTestElem))) {
				PUSH_O(heapPtr);
				goto JIT_IS_INSTANCE_end;
			}
		} else if (TYPE_ISARRAY(pTestType) && pToType == types[TYPE_SYSTEM_ARRAY_NO_TYPE]) {
			// every array is a System.Array (it was not recognised as one)
			PUSH_O(heapPtr);
			goto JIT_IS_INSTANCE_end;
		} else {
			if (Type_IsAssignableFrom(pToType, pTestType) ||
				(pToType->pGenericDefinition == types[TYPE_SYSTEM_NULLABLE] &&
				pToType->ppClassTypeArgs[0] == pTestType)) {
				// If derived class, interface, or nullable type compatible.
				PUSH_O(heapPtr);
				goto JIT_IS_INSTANCE_end;
			}
		}
		if (op == JIT_IS_INSTANCE) {
			PUSH_O(NULL);
		} else {
			THROW(types[TYPE_SYSTEM_INVALIDCASTEXCEPTION]);
		}
	}
JIT_IS_INSTANCE_end:
JIT_CAST_CLASS_end:
	GO_NEXT();

JIT_NEW_VECTOR_start: // Array with 1 dimension, zero-based
	OPCODE_USE(JIT_NEW_VECTOR);
	{
		tMD_TypeDef *pArrayTypeDef;
		U32 numElements;
		HEAP_PTR heapPtr;

		pArrayTypeDef = (tMD_TypeDef*)GET_OP();
		numElements = POP_U32();
		heapPtr = SystemArray_NewVector(pArrayTypeDef, numElements);
		PUSH_O(heapPtr);
		// Run any pending Finalizers
		RUN_FINALIZER();
	}
JIT_NEW_VECTOR_end:
	GO_NEXT();

JIT_LOAD_VECTOR_LEN_start: // Load the length of a vector array
	OPCODE_USE(JIT_LOAD_VECTOR_LEN);
	{
		PTR heapPtr = POP_O();
		if (heapPtr == NULL) { THROW_NULLREF(); }
		U32 value = SystemArray_GetLength(heapPtr);
		PUSH_U32(value);
	}
JIT_LOAD_VECTOR_LEN_end:
	GO_NEXT();

JIT_LOAD_ELEMENT_I8_start:
	OPCODE_USE(JIT_LOAD_ELEMENT_I8);
	{
		U32 value, idx = POP_U32(); // Array index
		HEAP_PTR heapPtr = POP_O();
		CHECK_ARRAY(heapPtr, idx);
		SystemArray_LoadElement(heapPtr, idx, (PTR)&value);
		PUSH_U32((I8)value);
	}
JIT_LOAD_ELEMENT_I8_end:
	GO_NEXT();

JIT_LOAD_ELEMENT_U8_start:
	OPCODE_USE(JIT_LOAD_ELEMENT_U8);
	{
		U32 value, idx = POP_U32(); // Array index
		HEAP_PTR heapPtr = POP_O();
		CHECK_ARRAY(heapPtr, idx);
		SystemArray_LoadElement(heapPtr, idx, (PTR)&value);
		PUSH_U32((U8)value);
	}
JIT_LOAD_ELEMENT_U8_end:
	GO_NEXT();

JIT_LOAD_ELEMENT_I16_start:
	OPCODE_USE(JIT_LOAD_ELEMENT_I16);
	{
		U32 value, idx = POP_U32(); // Array index
		HEAP_PTR heapPtr = POP_O();
		CHECK_ARRAY(heapPtr, idx);
		SystemArray_LoadElement(heapPtr, idx, (PTR)&value);
		PUSH_U32((I16)value);
	}
JIT_LOAD_ELEMENT_I16_end:
	GO_NEXT();

JIT_LOAD_ELEMENT_U16_start:
	OPCODE_USE(JIT_LOAD_ELEMENT_U16);
	{
		U32 value, idx = POP_U32(); // Array index
		HEAP_PTR heapPtr = POP_O();
		CHECK_ARRAY(heapPtr, idx);
		SystemArray_LoadElement(heapPtr, idx, (PTR)&value);
		PUSH_U32((U16)value);
	}
JIT_LOAD_ELEMENT_U16_end:
	GO_NEXT();

JIT_LOAD_ELEMENT_I32_start:
JIT_LOAD_ELEMENT_U32_start:
JIT_LOAD_ELEMENT_R32_start:
	OPCODE_USE(JIT_LOAD_ELEMENT_I32);
	{
		U32 value, idx = POP_U32(); // Array index
		HEAP_PTR heapPtr = POP_O();
		CHECK_ARRAY(heapPtr, idx);
		SystemArray_LoadElement(heapPtr, idx, (PTR)&value);
		PUSH_U32(value);
	}
JIT_LOAD_ELEMENT_I32_end:
JIT_LOAD_ELEMENT_U32_end:
JIT_LOAD_ELEMENT_R32_end:
	GO_NEXT();

JIT_LOAD_ELEMENT_I64_start:
JIT_LOAD_ELEMENT_R64_start:
	OPCODE_USE(JIT_LOAD_ELEMENT_I64);
	{
		U32 idx = POP_U32(); // array index
		HEAP_PTR heapPtr = POP_O();
		CHECK_ARRAY(heapPtr, idx);
		U64 value;
		SystemArray_LoadElement(heapPtr, idx, (PTR)&value);
		PUSH_U64(value);
	}
JIT_LOAD_ELEMENT_I64_end:
JIT_LOAD_ELEMENT_R64_end:
	GO_NEXT();

JIT_LOAD_ELEMENT_start:
	OPCODE_USE(JIT_LOAD_ELEMENT);
	{
		U32 idx = POP_U32(); // Array index
		HEAP_PTR heapPtr = POP_O(); // array object
		CHECK_ARRAY(heapPtr, idx);
		U32 size = GET_OP(); // size of type on stack
		*(U32*)pCurEvalStack = 0; // This is required to zero out the stack for types that are stored in <4 bytes in arrays
		SystemArray_LoadElement(heapPtr, idx, pCurEvalStack);
		pCurEvalStack += size;
	}
JIT_LOAD_ELEMENT_end:
	GO_NEXT();

JIT_LOAD_ELEMENT_ADDR_start:
	OPCODE_USE(JIT_LOAD_ELEMENT_ADDR);
	{
		U32 idx = POP_U32(); // Array index
		PTR heapPtr = POP_O();
		CHECK_ARRAY(heapPtr, idx);
		PTR pMem = SystemArray_LoadElementAddress(heapPtr, idx);
		PUSH_PTR(pMem);
	}
JIT_LOAD_ELEMENT_ADDR_end:
	GO_NEXT();

JIT_LOAD_ELEMENT_PTR_start:
	OPCODE_USE(JIT_LOAD_ELEMENT_PTR);
	{
		PTR value;
		U32 idx = POP_U32(); // Array index
		HEAP_PTR heapPtr = POP_O();
		CHECK_ARRAY(heapPtr, idx);
		value = ((PTR*)((tSystemArray*)heapPtr)->elements)[idx];      // (the elements are pointer-sized: no need for the general load)
		PUSH_PTR(value);
	}
JIT_LOAD_ELEMENT_PTR_end:
	GO_NEXT();

JIT_STORE_ELEMENT_PTR_start:
	OPCODE_USE(JIT_STORE_ELEMENT_PTR);
	{
		PTR value = POP_PTR(); // Value
		U32 idx = POP_U32(); // Array index
		PTR heapPtr = POP_O();
		CHECK_ARRAY(heapPtr, idx);
		SystemArray_StoreElement(heapPtr, idx, (PTR)&value);
	}
JIT_STORE_ELEMENT_PTR_end:
	GO_NEXT();

JIT_STORE_ELEMENT_32_start:
	OPCODE_USE(JIT_STORE_ELEMENT_32);
	{
		U32 value = POP_U32(); // Value
		U32 idx = POP_U32(); // Array index
		PTR heapPtr = POP_O();
		CHECK_ARRAY(heapPtr, idx);
		SystemArray_StoreElement(heapPtr, idx, (PTR)&value);
	}
JIT_STORE_ELEMENT_32_end:
	GO_NEXT();

JIT_STORE_ELEMENT_64_start:
	OPCODE_USE(JIT_STORE_ELEMENT_64);
	{
		U64 value = POP_U64(); // Value
		U32 idx = POP_U32(); // Array index
		PTR heapPtr = POP_O();
		CHECK_ARRAY(heapPtr, idx);
		SystemArray_StoreElement(heapPtr, idx, (PTR)&value);
	}
JIT_STORE_ELEMENT_64_end:
	GO_NEXT();

JIT_STORE_ELEMENT_start:
	OPCODE_USE(JIT_STORE_ELEMENT);
	{
		HEAP_PTR heapPtr;
		PTR pMem;
		U32 idx, size = GET_OP(); // Size in bytes of value on stack
		POP(size);
		pMem = pCurEvalStack;
		idx = POP_U32(); // Array index
		heapPtr = POP_O(); // Array on heap
		CHECK_ARRAY(heapPtr, idx);
		SystemArray_StoreElement(heapPtr, idx, pMem);
	}
JIT_STORE_ELEMENT_end:
	GO_NEXT();

JIT_STOREFIELD_O_start:
JIT_STOREFIELD_INTNATIVE_start:
JIT_STOREFIELD_PTR_start:
	OPCODE_USE(JIT_STOREFIELD_PTR);
	{
		// pointer-sized (these were aliased to the 4-byte store below, "only for 32-bit")
		tMD_FieldDef *pFieldDef;
		PTR pMem;
		PTR value;
		HEAP_PTR heapPtr;

		pFieldDef = (tMD_FieldDef*)GET_OP();
		value = POP_PTR();
		heapPtr = POP_O();
		if (heapPtr == NULL) {
			THROW_NULLREF();
		}
		pMem = heapPtr + pFieldDef->memOffset;
		*(PTR*)pMem = value;
	}
JIT_STOREFIELD_O_end:
JIT_STOREFIELD_INTNATIVE_end:
JIT_STOREFIELD_PTR_end:
	GO_NEXT();

JIT_STOREFIELD_INT32_start:
JIT_STOREFIELD_F32_start:
	OPCODE_USE(JIT_STOREFIELD_INT32);
	{
		tMD_FieldDef *pFieldDef;
		PTR pMem;
		U32 value;
		HEAP_PTR heapPtr;

		pFieldDef = (tMD_FieldDef*)GET_OP();
		value = POP_U32();
		heapPtr = POP_O();
		if (heapPtr == NULL) {
			THROW_NULLREF();
		}
		pMem = heapPtr + pFieldDef->memOffset;
		*(U32*)pMem = value;
	}
JIT_STOREFIELD_INT32_end:
JIT_STOREFIELD_F32_end:
	GO_NEXT();

JIT_STOREFIELD_INT64_start:
JIT_STOREFIELD_F64_start:
	OPCODE_USE(JIT_STOREFIELD_F64);
	{
		tMD_FieldDef *pFieldDef;
		PTR pMem;
		U64 value;
		HEAP_PTR heapPtr;

		pFieldDef = (tMD_FieldDef*)GET_OP();
		value = POP_U64();
		heapPtr = POP_O();
		if (heapPtr == NULL) {
			THROW_NULLREF();
		}
		pMem = heapPtr + pFieldDef->memOffset;
		*(U64*)pMem = value;
	}
JIT_STOREFIELD_INT64_end:
JIT_STOREFIELD_F64_end:
	GO_NEXT();

JIT_STOREFIELD_VALUETYPE_start:
	OPCODE_USE(JIT_STOREFIELD_VALUETYPE);
	{
		tMD_FieldDef *pFieldDef;
		PTR pMem;

		pFieldDef = (tMD_FieldDef*)GET_OP();
		pCurEvalStack -= pFieldDef->memSize;
		pMem = pCurEvalStack;
		heapPtr = POP_O();
		if (heapPtr == NULL) {
			THROW_NULLREF();
		}
		memcpy(heapPtr + pFieldDef->memOffset, pMem, pFieldDef->memSize);
	}
JIT_STOREFIELD_VALUETYPE_end:
	GO_NEXT();

JIT_LOADFIELD_start:
	OPCODE_USE(JIT_LOADFIELD);
	// TODO: Optimize into LOADFIELD of different types O, INT32, INT64, F, etc...)
	{
		tMD_FieldDef *pFieldDef;

		pFieldDef = (tMD_FieldDef*)GET_OP();
		heapPtr = POP_O();
		if (heapPtr == NULL) {
			THROW_NULLREF();
		}
		pMem = heapPtr + pFieldDef->memOffset;
		// It may not be a value-type, but this'll work anyway
		PUSH_VALUETYPE(pMem, pFieldDef->memSize, pFieldDef->memSize);
	}
JIT_LOADFIELD_end:
	GO_NEXT();

JIT_LOADFIELD_4_start:
	OPCODE_USE(JIT_LOADFIELD_4);
	{
		U32 ofs = GET_OP();
		PTR heapPtr = POP_O();
		if (heapPtr == NULL) {
			THROW_NULLREF();
		}
		PUSH_U32(*(U32*)(heapPtr + ofs));
	}
JIT_LOADFIELD_4_end:
	GO_NEXT();

JIT_LOADFIELD_VALUETYPE_start:
	OPCODE_USE(JIT_LOADFIELD_VALUETYPE);
	{
		tMD_FieldDef *pFieldDef;

		u32Value = GET_OP(); // Get the size of the value-type on the eval stack
		pFieldDef = (tMD_FieldDef*)GET_OP();
		
		// [Steve edit] The following line used to be:
		//     pCurrentMethodState->stackOfs -= u32Value;
		// ... but this seems to result in calculating the wrong pMem value and getting garbage results.
		// My guess is that at some point they refactored from using 'pEvalStack' to 'pCurEvalStack', but
		// didn't update this method (because nothing in corlib reads fields from structs).
		// I think the following line moves the stack pointer along correctly instead:
		pCurEvalStack -= u32Value;
		
		//pMem = pEvalStack + pCurrentMethodState->stackOfs + pFieldDef->memOffset;
		pMem = pCurEvalStack + pFieldDef->memOffset;
		// It may not be a value-type, but this'll work anyway
		PUSH_VALUETYPE(pMem, pFieldDef->memSize, pFieldDef->memSize);
	}
JIT_LOADFIELD_VALUETYPE_end:
	GO_NEXT();

JIT_LOAD_FIELD_ADDR_start:
	OPCODE_USE(JIT_LOAD_FIELD_ADDR);
	{
		U32 ofs = GET_OP();
		HEAP_PTR heapPtr = POP_O();
		if (heapPtr == NULL) {
			THROW_NULLREF();
		}
		PTR pMem = heapPtr + ofs;
		PUSH_PTR(pMem);
	}
JIT_LOAD_FIELD_ADDR_end:
	GO_NEXT();

JIT_STORESTATICFIELD_O_start:
JIT_STORESTATICFIELD_INTNATIVE_start:
JIT_STORESTATICFIELD_PTR_start:
	OPCODE_USE(JIT_STORESTATICFIELD_PTR);
	{
		// pointer-sized (these were aliased to the 4-byte store below, "only for 32-bit")
		tMD_FieldDef *pFieldDef = (tMD_FieldDef*)GET_OP();
		PTR value = POP_PTR();
		*(PTR*)pFieldDef->pMemory = value;
	}
JIT_STORESTATICFIELD_O_end:
JIT_STORESTATICFIELD_INTNATIVE_end:
JIT_STORESTATICFIELD_PTR_end:
	GO_NEXT();

JIT_STORESTATICFIELD_INT32_start:
JIT_STORESTATICFIELD_F32_start:
	OPCODE_USE(JIT_STORESTATICFIELD_INT32);
	{
		tMD_FieldDef *pFieldDef;
		PTR pMem;
		U32 value;

		pFieldDef = (tMD_FieldDef*)GET_OP();
		value = POP_U32();
		pMem = pFieldDef->pMemory;
		*(U32*)pMem = value;
	}
JIT_STORESTATICFIELD_INT32_end:
JIT_STORESTATICFIELD_F32_end:
	GO_NEXT();

JIT_STORESTATICFIELD_F64_start:
JIT_STORESTATICFIELD_INT64_start:
	OPCODE_USE(JIT_STORESTATICFIELD_INT64);
	{
		tMD_FieldDef *pFieldDef;
		PTR pMem;
		U64 value;

		pFieldDef = (tMD_FieldDef*)GET_OP();
		value = POP_U64();
		//pMem = pFieldDef->pParentType->pStaticFields + pFieldDef->memOffset;
		pMem = pFieldDef->pMemory;
		*(U64*)pMem = value;
	}
JIT_STORESTATICFIELD_F64_end:
JIT_STORESTATICFIELD_INT64_end:
	GO_NEXT();

JIT_STORESTATICFIELD_VALUETYPE_start:
	OPCODE_USE(JIT_STORESTATICFIELD_VALUETYPE);
	{
		tMD_FieldDef *pFieldDef;
		PTR pMem;

		pFieldDef = (tMD_FieldDef*)GET_OP();
		pMem = pFieldDef->pMemory;
		POP_VALUETYPE(pMem, pFieldDef->memSize, pFieldDef->memSize);
	}
JIT_STORESTATICFIELD_VALUETYPE_end:
	GO_NEXT();

JIT_LOADSTATICFIELDADDRESS_CHECKTYPEINIT_start:
	op = JIT_LOADSTATICFIELDADDRESS_CHECKTYPEINIT;
	goto loadStaticFieldStart;
JIT_LOADSTATICFIELD_CHECKTYPEINIT_VALUETYPE_start:
	op = JIT_LOADSTATICFIELD_CHECKTYPEINIT_VALUETYPE;
	goto loadStaticFieldStart;
JIT_LOADSTATICFIELD_CHECKTYPEINIT_F64_start:
JIT_LOADSTATICFIELD_CHECKTYPEINIT_INT64_start:
	// (INT64 had no handler at all, so reading a static long crashed the JIT: "Opcode not available".
	// It is the same 8-byte copy as a double.)
	op = JIT_LOADSTATICFIELD_CHECKTYPEINIT_F64;
	goto loadStaticFieldStart;
JIT_LOADSTATICFIELD_CHECKTYPEINIT_O_start:
	op = JIT_LOADSTATICFIELD_CHECKTYPEINIT_O;
	goto loadStaticFieldStart;
JIT_LOADSTATICFIELD_CHECKTYPEINIT_INTNATIVE_start:
	op = JIT_LOADSTATICFIELD_CHECKTYPEINIT_INTNATIVE;
	goto loadStaticFieldStart;
JIT_LOADSTATICFIELD_CHECKTYPEINIT_PTR_start:
	op = JIT_LOADSTATICFIELD_CHECKTYPEINIT_PTR;
	goto loadStaticFieldStart;
JIT_LOADSTATICFIELD_CHECKTYPEINIT_INT32_start:
JIT_LOADSTATICFIELD_CHECKTYPEINIT_F32_start:
	op = 0;
loadStaticFieldStart:
	OPCODE_USE(JIT_LOADSTATICFIELD_CHECKTYPEINIT_INT32);
	{
		tMD_FieldDef *pFieldDef;
		tMD_TypeDef *pParentType;

		pFieldDef = (tMD_FieldDef*)GET_OP();
		pParentType = pFieldDef->pParentType;
		// Check that any type (static) constructor has been called
		if (pParentType->isTypeInitialised == 0) {
			// Set the state to initialised
			pParentType->isTypeInitialised = 1;
			// Initialise the type (if there is a static constructor)
			if (pParentType->pStaticConstructor != NULL) {
				tMethodState *pCallMethodState;

				// Call static constructor
				// Need to re-run this instruction when we return from static constructor call
				//pCurrentMethodState->ipOffset -= 2;
				pCurOp -= 2;
				pCallMethodState = MethodState_Direct(pThread, pParentType->pStaticConstructor, pCurrentMethodState, 0);
				// There can be no parameters, so don't need to set them up
				CHANGE_METHOD_STATE(pCallMethodState);
				GO_NEXT_CHECK();
			}
		}
		if (op == JIT_LOADSTATICFIELD_CHECKTYPEINIT_F64) {
			U64 value;
			value = *(U64*)(pFieldDef->pMemory);
			PUSH_U64(value);
		} else if (op == JIT_LOADSTATICFIELD_CHECKTYPEINIT_VALUETYPE) {
			PUSH_VALUETYPE(pFieldDef->pMemory, pFieldDef->memSize, pFieldDef->memSize);
		} else if (op == JIT_LOADSTATICFIELDADDRESS_CHECKTYPEINIT) {
			// the address of the field: a pointer (this was cast to U32 and pushed as 4 bytes)
			PUSH_PTR(pFieldDef->pMemory);
		} else if (op == JIT_LOADSTATICFIELD_CHECKTYPEINIT_O ||
				op == JIT_LOADSTATICFIELD_CHECKTYPEINIT_INTNATIVE ||
				op == JIT_LOADSTATICFIELD_CHECKTYPEINIT_PTR) {
			// references, native ints and managed pointers are pointer-sized
			PUSH_PTR(*(PTR*)pFieldDef->pMemory);
		} else {
			U32 value = *(U32*)pFieldDef->pMemory;
			PUSH_U32(value);
		}
	}
JIT_LOADSTATICFIELDADDRESS_CHECKTYPEINIT_end:
JIT_LOADSTATICFIELD_CHECKTYPEINIT_VALUETYPE_end:
JIT_LOADSTATICFIELD_CHECKTYPEINIT_INT32_end:
JIT_LOADSTATICFIELD_CHECKTYPEINIT_F32_end:
JIT_LOADSTATICFIELD_CHECKTYPEINIT_F64_end:
JIT_LOADSTATICFIELD_CHECKTYPEINIT_INT64_end:
JIT_LOADSTATICFIELD_CHECKTYPEINIT_O_end:
JIT_LOADSTATICFIELD_CHECKTYPEINIT_INTNATIVE_end:
JIT_LOADSTATICFIELD_CHECKTYPEINIT_PTR_end:
	GO_NEXT();

JIT_INIT_VALUETYPE_start:
	OPCODE_USE(JIT_INIT_VALUETYPE);
	{
		tMD_TypeDef *pTypeDef;

		pTypeDef = (tMD_TypeDef*)GET_OP();
		pMem = POP_PTR();
		memset(pMem, 0, pTypeDef->instanceMemSize);
	}
JIT_INIT_VALUETYPE_end:
	GO_NEXT();

JIT_INIT_OBJECT_start:
	OPCODE_USE(JIT_INIT_OBJECT);
	{
		PTR pMem = POP_PTR();
		*(void**)pMem = NULL;
	}
JIT_INIT_OBJECT_end:
	GO_NEXT();

JIT_BOX_INTNATIVE_start:
JIT_BOX_PTR_start:      // an IntPtr read from a field has this stack type; boxing it is the same pointer-sized copy (it had no handler, so
                        // anything that boxed one, such as Delegate.Equals, failed to compile: "Opcode not available")
	OPCODE_USE(JIT_BOX_INTNATIVE);
	{
		tMD_TypeDef *pTypeDef = (tMD_TypeDef*)GET_OP();
		heapPtr = Heap_AllocType(pTypeDef);
		*(PTR*)heapPtr = POP_PTR();    // a native int is pointer-sized
		PUSH_O(heapPtr);
	}
JIT_BOX_INTNATIVE_end:
JIT_BOX_PTR_end:
	GO_NEXT();

JIT_BOX_INT32_start:
JIT_BOX_F32_start:
	OPCODE_USE(JIT_BOX_INT32);
	{
		tMD_TypeDef *pTypeDef;

		pTypeDef = (tMD_TypeDef*)GET_OP();
		heapPtr = Heap_AllocType(pTypeDef);
		u32Value = POP_U32();
		*(U32*)heapPtr = u32Value;
		PUSH_O(heapPtr);
	}
JIT_BOX_INT32_end:
JIT_BOX_F32_end:
	GO_NEXT();

JIT_BOX_INT64_start:
JIT_BOX_F64_start:
OPCODE_USE(JIT_BOX_INT64);
	{
		tMD_TypeDef *pTypeDef = (tMD_TypeDef*)GET_OP();
		heapPtr = Heap_AllocType(pTypeDef);
		*(U64*)heapPtr = POP_U64();
		PUSH_O(heapPtr);
	}
JIT_BOX_INT64_end:
JIT_BOX_F64_end:
	GO_NEXT();

JIT_BOX_VALUETYPE_start:
	OPCODE_USE(JIT_BOX_VALUETYPE);
	{
		tMD_TypeDef *pTypeDef;

		pTypeDef = (tMD_TypeDef*)GET_OP();
		heapPtr = Heap_AllocType(pTypeDef);
		POP_VALUETYPE(heapPtr, pTypeDef->stackSize, pTypeDef->stackSize);
		PUSH_O(heapPtr);
	}
JIT_BOX_VALUETYPE_end:
	GO_NEXT();

JIT_BOX_O_start:
	pCurOp++;
	// Fall-through
JIT_UNBOX2OBJECT_start: // TODO: This is not correct - it should check the type, just like CAST_CLASS
	OPCODE_USE(JIT_UNBOX2OBJECT);
	// Nothing to do
JIT_BOX_O_end:
JIT_UNBOX2OBJECT_end:
	GO_NEXT();

JIT_BOX_NULLABLE_start:
	OPCODE_USE(JIT_BOX_NULLABLE);
	{
		// Get the underlying type of the nullable type, and the nullable type itself
		tMD_TypeDef *pType = (tMD_TypeDef*)GET_OP();
		tMD_TypeDef *pNullable = (tMD_TypeDef*)GET_OP();

		// Take the nullable type off the stack: its whole size, which is the hasValue flag, any padding,
		// and .Value (not simply "+4": where .Value sits depends on its alignment)
		pCurEvalStack -= pNullable->stackSize;
		// If .HasValue
		if (*(U32*)pCurEvalStack) {
			// Box the underlying type
			HEAP_PTR boxed;
			boxed = Heap_Box(pType, pCurEvalStack + Type_NullableValueField(pNullable)->memOffset);
			PUSH_O(boxed);
		} else {
			// Put a NULL pointer on the stack
			PUSH_O(NULL);
		}
	}
JIT_BOX_NULLABLE_end:
	GO_NEXT();

JIT_UNBOX2VALUETYPE_start:
	OPCODE_USE(JIT_UNBOX2VALUETYPE);
	{
		// unbox.any of a value type. This used to take the size from the object's own type with no
		// checks, so null crashed and a wrongly-typed object was silently reinterpreted.
		tMD_TypeDef *pExpected = (tMD_TypeDef*)GET_OP();
		HEAP_PTR pBoxed = POP_O();
		if (pBoxed == NULL) {
			THROW_NULLREF();
		}
		if (!UnboxCompatible(pExpected, Heap_GetType(pBoxed))) {
			THROW(types[TYPE_SYSTEM_INVALIDCASTEXCEPTION]);
		}
		PUSH_VALUETYPE(pBoxed, pExpected->stackSize, pExpected->stackSize);
	}
JIT_UNBOX2VALUETYPE_end:
	GO_NEXT();

JIT_UNBOX_start:
	OPCODE_USE(JIT_UNBOX);
	{
		tMD_TypeDef *pExpected = (tMD_TypeDef*)GET_OP();
		HEAP_PTR pBoxed = POP_O();
		if (pBoxed == NULL) {
			THROW_NULLREF();
		}
		if (!UnboxCompatible(pExpected, Heap_GetType(pBoxed))) {
			THROW(types[TYPE_SYSTEM_INVALIDCASTEXCEPTION]);
		}
		// the value lives at the start of the box's memory
		PUSH_PTR(pBoxed);
	}
JIT_UNBOX_end:
	GO_NEXT();

JIT_UNBOX_NULLABLE_start:
	OPCODE_USE(JIT_UNBOX_NULLABLE);
	{
		tMD_TypeDef *pTypeDef = (tMD_TypeDef*)GET_OP();
		tMD_TypeDef *pNullable = (tMD_TypeDef*)GET_OP();
		HEAP_PTR heapPtr;
		heapPtr = POP_O();
		// Build the whole Nullable<T> on the stack: zero it (so padding is clean), then set hasValue and .Value
		memset(pCurEvalStack, 0, pNullable->stackSize);
		if (heapPtr != NULL) {
			*(U32*)pCurEvalStack = 1;      // .HasValue
			memcpy(pCurEvalStack + Type_NullableValueField(pNullable)->memOffset, heapPtr, pTypeDef->stackSize);
		}
		pCurEvalStack += pNullable->stackSize;
	}
JIT_UNBOX_NULLABLE_end:
	GO_NEXT();

JIT_LOADTOKEN_TYPE_start:
	OPCODE_USE(JIT_LOADTOKEN_TYPE);
	{
		tMD_TypeDef *pTypeDef;

		pTypeDef = (tMD_TypeDef*)GET_OP();
		// Push new valuetype onto evaluation stack
		PUSH_PTR((PTR)pTypeDef);
	}
JIT_LOADTOKEN_TYPE_end:
	GO_NEXT();

JIT_LOADTOKEN_FIELD_start:
	OPCODE_USE(JIT_LOADTOKEN_FIELD);
	{
		tMD_FieldDef *pFieldDef;

		pFieldDef = (tMD_FieldDef*)GET_OP();
		// Push new valuetype onto evaluation stack - only works on static fields.
		PUSH_PTR(pFieldDef->pMemory);
	}
JIT_LOADTOKEN_FIELD_end:
	GO_NEXT();

JIT_RETHROW_start:
	op = JIT_RETHROW;
	goto throwStart;
JIT_THROW_start:
	op = JIT_THROW;
throwStart:
	OPCODE_USE(JIT_THROW);
	// Get the exception object
	if (op == JIT_RETHROW) {
		heapPtr = pThread->pCurrentExceptionObject;
	} else {
		heapPtr = POP_O();
throwHeapPtr:
		pThread->pCurrentExceptionObject = heapPtr;
	}
	SAVE_METHOD_STATE();
	// A new search starts in the frame that threw. If a filter is running, the exception is one
	// thrown from inside it, and nothing outside that filter's own call chain may handle it.
	pThread->pThrowTopState = pCurrentMethodState;
	pThread->pSearchFrame = pCurrentMethodState;
	pThread->searchNextClause = 0;
	pThread->searchInFilter = (pThread->numFilters > 0);

	// ---- First pass: find the handler. Walks the frames from the throwing one down, in each testing
	// the clauses that cover where the frame is. A filter clause means running the filter's code to
	// get its verdict: the search then stops here and is resumed by endfilter (JIT_ENDFILTER), which
	// is why where it has got to lives in the thread rather than in locals.
searchHandlers:
	{
		U32 i, startClause;
		tMethodState *pFrame;
		tExceptionHeader *pCatch;
		tMD_TypeDef *pExType;

		pExType = Heap_GetType(pThread->pCurrentExceptionObject);
		pCatch = NULL;
		pFrame = pThread->pSearchFrame;
		startClause = pThread->searchNextClause;
		for(;;) {
			if (pThread->searchInFilter && pFrame == pThread->pFilterCtx[pThread->numFilters - 1].pFrame) {
				// Reached the frame running the filter that this exception came out of, and nothing
				// in between handled it: the filter threw, which counts as it answering "no".
				// (Unwinding to the filter's frame, with a NULL handler, means exactly that.)
				pThread->pCatchMethodState = pFrame;
				pThread->pCatchExceptionHandler = NULL;
				pThread->nextFinallyUnwindStack = 0;
				goto unwindToHandler;
			}
			for (i=startClause; i<pFrame->pMethod->pJITted->numExceptionHandlers; i++) {
				tExceptionHeader *pEx = &pFrame->pMethod->pJITted->pExceptionHeaders[i];
				if (pFrame->ipOffset - 1 >= pEx->tryStart && pFrame->ipOffset - 1 < pEx->tryEnd) {
					if (pEx->flags == COR_ILEXCEPTION_CLAUSE_EXCEPTION &&
						Type_IsDerivedFromOrSame(pEx->u.pCatchTypeDef, pExType)) {
						// Found the correct catch clause to jump to
						pCatch = pEx;
						break;
					}
					if (pEx->flags == COR_ILEXCEPTION_CLAUSE_FILTER) {
						// Run the filter code in this frame, with just the exception on its stack,
						// leaving the frames above (which still have finally blocks to run) alone.
						tFilterCtx *pCtx;
						if (pThread->numFilters >= MAX_ACTIVE_FILTERS) {
							Crash("Exception filters nested more than %d deep", MAX_ACTIVE_FILTERS);
						}
						if (pThread->pFilterCtx == NULL) {
							pThread->pFilterCtx = (tFilterCtx*)malloc(MAX_ACTIVE_FILTERS * sizeof(tFilterCtx));
						}
						pCtx = &pThread->pFilterCtx[pThread->numFilters++];
						pCtx->pTopState = pThread->pThrowTopState;
						pCtx->pFrame = pFrame;
						pCtx->clause = i;
						pCtx->savedIp = pFrame->ipOffset;
						pCtx->savedStackOfs = pFrame->stackOfs;
						pCtx->savedException = pThread->pCurrentExceptionObject;
						pCtx->outerSearchInFilter = pThread->searchInFilter;
						pFrame->stackOfs = 0;
						pFrame->ipOffset = pEx->u.filterOffset;
						pThread->pCurrentMethodState = pFrame;
						LOAD_METHOD_STATE();
						PUSH_O(pThread->pCurrentExceptionObject);
						goto throwEnd;
					}
				}
			}
			if (pCatch != NULL) {
				// Found a suitable exception handler
				break;
			}
			pFrame = pFrame->pCaller;
			startClause = 0;
			if (pFrame == NULL) {
				Crash("Unhandled exception in %s.%s(): %s.%s",
					pCurrentMethodState->pMethod->pParentType->name,
					pCurrentMethodState->pMethod->name, pExType->nameSpace, pExType->name);
			}
		}
		pThread->pCatchMethodState = pFrame;
		pThread->pCatchExceptionHandler = pCatch;
		pThread->nextFinallyUnwindStack = 0;
	}

	// ---- Second pass: unwind the stack down to the handler's frame, running the finally and fault
	// clauses on the way, then enter the handler. Resumed by END_FINALLY after each such block, so
	// it works from pThread->pCatchMethodState and not from locals.
unwindToHandler:
	{
		U32 i;

		// This loop also visits the catching frame itself. There, only the finally clauses that
		// come *before* the matching catch in the exception table are run: the table lists
		// nested clauses before the ones enclosing them, so those are the finally blocks sitting
		// inside the catch's try block (they must run before the catch handler), whereas a
		// finally attached to the same try as the catch comes after it and runs later, at LEAVE.
		// Without this, `try { try { throw } finally { A } } catch { C }` in one method skipped A.
		// NB: no initialisers on these declarations; `goto finallyUnwindStack` jumps past them.
		for (;;) {
			tMethodState *pPrevState;
			U32 finallyLimit;

finallyUnwindStack:
			// (A NULL handler is a filter that threw: its frame has no clause to stop short of.)
			finallyLimit = (pCurrentMethodState == pThread->pCatchMethodState)
				? ((pThread->pCatchExceptionHandler != NULL)
					? (U32)(pThread->pCatchExceptionHandler - pCurrentMethodState->pMethod->pJITted->pExceptionHeaders)
					: 0)
				: pCurrentMethodState->pMethod->pJITted->numExceptionHandlers;
			for (i=pThread->nextFinallyUnwindStack; i<finallyLimit; i++) {
				tExceptionHeader *pEx;

				pEx = &pCurrentMethodState->pMethod->pJITted->pExceptionHeaders[i];
				if ((pEx->flags == COR_ILEXCEPTION_CLAUSE_FINALLY || pEx->flags == COR_ILEXCEPTION_CLAUSE_FAULT) &&
					pCurrentMethodState->ipOffset - 1 >= pEx->tryStart &&
					pCurrentMethodState->ipOffset - 1 < pEx->tryEnd) {

					// Found a finally (or fault) handler. Make this frame the thread's current one, empty
					// its evaluation stack, point it at the handler, and only then load: the order
					// matters, because LOAD_METHOD_STATE derives pCurOp from ipOffset. (This used
					// to CHANGE_METHOD_STATE first and set ipOffset afterwards, so the handler
					// address was written after pCurOp had been computed and the finally block
					// never ran; and its SAVE half stored the innermost frame's op pointer into
					// a caller's frame state when unwinding more than one frame.)
					pThread->pCurrentMethodState = pCurrentMethodState;
					pCurrentMethodState->stackOfs = 0;
					pCurrentMethodState->ipOffset = pEx->handlerStart;
					LOAD_METHOD_STATE();
					// Keep track of which finally clause should be executed next
					pThread->nextFinallyUnwindStack = i + 1;
					goto throwEnd;
				}
			}

			if (pCurrentMethodState == pThread->pCatchMethodState) {
				// Reached the catching frame with every nested finally run. Clear the tracker so a
				// later, ordinary END_FINALLY isn't mistaken for part of this unwind.
				pThread->nextFinallyUnwindStack = 0;
				break;
			}

			pPrevState = pCurrentMethodState->pCaller;
			MethodState_Delete(pThread, &pCurrentMethodState);
			pCurrentMethodState = pPrevState;
			// Reset the stack unwind tracker
			pThread->nextFinallyUnwindStack = 0;
		}

		if (pThread->pCatchExceptionHandler == NULL) {
			// The unwind was of an exception thrown inside a filter, ending at the filter's frame:
			// the filter answers "no". Put everything back as it was when the filter began, and
			// carry on looking for the handler of the exception being filtered, after this clause.
			tFilterCtx *pCtx = &pThread->pFilterCtx[--pThread->numFilters];
			pCtx->pFrame->ipOffset = pCtx->savedIp;
			pCtx->pFrame->stackOfs = pCtx->savedStackOfs;
			pThread->pCurrentExceptionObject = pCtx->savedException;
			pThread->pThrowTopState = pCtx->pTopState;
			pThread->searchInFilter = pCtx->outerSearchInFilter;
			pThread->pSearchFrame = pCtx->pFrame;
			pThread->searchNextClause = pCtx->clause + 1;
			pThread->pCurrentMethodState = pCtx->pTopState;
			LOAD_METHOD_STATE();
			goto searchHandlers;
		}

		// Set the IP to the catch handler
		pCurrentMethodState->ipOffset = pThread->pCatchExceptionHandler->handlerStart;
		// Set the current method state. The unwind loop above only advanced the local
		// pCurrentMethodState; LOAD_METHOD_STATE reloads from the thread, so without this it
		// restored the (already deleted) frame that threw. That "worked" for a one-frame unwind
		// only because that frame's leftover `ret` then returned into the catching frame, and
		// ran off the end of the opcode stream for anything deeper. It also left the GC walking
		// dead frames when it scanned for roots.
		pThread->pCurrentMethodState = pCurrentMethodState;
		LOAD_METHOD_STATE();
		// Push onto this stack-frame's evaluation stack the opject thrown
		POP_ALL();
		PUSH_O(pThread->pCurrentExceptionObject);
	}
throwEnd:
JIT_THROW_end:
JIT_RETHROW_end:
	GO_NEXT_CHECK();

throwNullRef:
	throwEx = types[TYPE_SYSTEM_NULLREFERENCEEXCEPTION];
throwTyped:
	heapPtr = Heap_AllocType(throwEx);
	goto throwHeapPtr;

// The fused instructions (generated by tools/gen_fused_ops.py)
#define FUSED_HANDLERS
#include "JIT_Fused.gen.h"
#undef FUSED_HANDLERS

// stelem.i1, .i2, .i4/.r4: store the low 8, 16 or all 32 bits of the value
JIT_STORE_ELEMENT_I1_start:
	OPCODE_USE(JIT_STORE_ELEMENT_I1);
	{
		U32 value = POP_U32(); // Value
		U32 idx = POP_U32(); // Array index
		PTR heapPtr = POP_O();
		CHECK_ARRAY(heapPtr, idx);
		((U8*)((tSystemArray*)heapPtr)->elements)[idx] = (U8)value;
	}
JIT_STORE_ELEMENT_I1_end:
	GO_NEXT();

JIT_STORE_ELEMENT_I2_start:
	OPCODE_USE(JIT_STORE_ELEMENT_I2);
	{
		U32 value = POP_U32(); // Value
		U32 idx = POP_U32(); // Array index
		PTR heapPtr = POP_O();
		CHECK_ARRAY(heapPtr, idx);
		((U16*)((tSystemArray*)heapPtr)->elements)[idx] = (U16)value;
	}
JIT_STORE_ELEMENT_I2_end:
	GO_NEXT();

JIT_STORE_ELEMENT_I4_start:
	OPCODE_USE(JIT_STORE_ELEMENT_I4);
	{
		U32 value = POP_U32(); // Value
		U32 idx = POP_U32(); // Array index
		PTR heapPtr = POP_O();
		CHECK_ARRAY(heapPtr, idx);
		((U32*)((tSystemArray*)heapPtr)->elements)[idx] = value;
	}
JIT_STORE_ELEMENT_I4_end:
	GO_NEXT();

// ldelem.u1 of a byte element, and of a bool element (which takes 4 bytes in an array)
JIT_LOAD_ELEMENT_U8_1_start:
	OPCODE_USE(JIT_LOAD_ELEMENT_U8_1);
	{
		U32 idx = POP_U32(); // Array index
		PTR heapPtr = POP_O();
		CHECK_ARRAY(heapPtr, idx);
		PUSH_U32(((U8*)((tSystemArray*)heapPtr)->elements)[idx]);
	}
JIT_LOAD_ELEMENT_U8_1_end:
	GO_NEXT();

JIT_LOAD_ELEMENT_U8_4_start:
	OPCODE_USE(JIT_LOAD_ELEMENT_U8_4);
	{
		U32 idx = POP_U32(); // Array index
		PTR heapPtr = POP_O();
		CHECK_ARRAY(heapPtr, idx);
		PUSH_U32((U8)((U32*)((tSystemArray*)heapPtr)->elements)[idx]);
	}
JIT_LOAD_ELEMENT_U8_4_end:
	GO_NEXT();

// ldelema: the address of an element, whose size the instruction carries
JIT_LOAD_ELEMENT_ADDR_N_start:
	OPCODE_USE(JIT_LOAD_ELEMENT_ADDR_N);
	{
		U32 size = GET_OP();
		U32 idx = POP_U32(); // Array index
		PTR heapPtr = POP_O();
		CHECK_ARRAY(heapPtr, idx);
		PUSH_PTR(((tSystemArray*)heapPtr)->elements + (size_t)idx * size);
	}
JIT_LOAD_ELEMENT_ADDR_N_end:
	GO_NEXT();

JIT_NATIVE_BLOCK_start:
	OPCODE_USE(JIT_NATIVE_BLOCK);
	{
#if NATIVE_BLOCKS
		void *code = (void*)GET_OP();
		U32 numExits = (U32)GET_OP();
		tOpWord *exitTargets = pCurOp;               // the CIL-offset operands of the exits follow
		PTR blockSp = pCurEvalStack;
		U32 status = NativeBlock_RunStraight(code, pParamsLocals, &blockSp);
		pCurEvalStack = blockSp;
		pCurOp += numExits;
		if (status != NB_DONE) {
			if (status == NB_NULLREF) {
				THROW_NULLREF();
			}
			if (status == NB_INDEXRANGE) {
				THROW(types[TYPE_SYSTEM_INDEXOUTOFRANGEEXCEPTION]);
			}
			// an exit: carry on at its target
			pCurOp = pOps + exitTargets[status - NB_EXIT];
			GO_NEXT_CHECK();
		}
#else
		Crash("a native block was run on a target without native blocks");
#endif
	}
JIT_NATIVE_BLOCK_end:
	GO_NEXT();

JIT_NATIVE_LOOP_start:
	OPCODE_USE(JIT_NATIVE_LOOP);
	{
#if NATIVE_BLOCKS
		tOpWord *blockOp = pCurOp - 1;               // this instruction, for running it again
		void *code = (void*)GET_OP();
		U32 numExits = (U32)GET_OP();
		tOpWord *exitTargets = pCurOp;               // the CIL-offset operands of the exits follow
		PTR blockSp = pCurEvalStack;
		// The thread's time slice counts branches. Native loops count their backward branches against what is left of it,
		// and when it runs out the block hands control back so that other threads get their turn.
		U32 budget = numInst;
		U32 entry = pCurrentMethodState->nativeEntry;      // 0, unless this block gave up the processor in a loop
		U32 status;
		pCurrentMethodState->nativeEntry = 0;
		status = NativeBlock_Run(code, pParamsLocals, &blockSp, &budget, entry);
		pCurEvalStack = blockSp;
		pCurOp += numExits;
		if (status == NB_DONE) {
			numInst = budget > 0 ? budget : 1;
		} else if (status == NB_NULLREF) {
			THROW_NULLREF();
		} else if (status == NB_INDEXRANGE) {
			THROW(types[TYPE_SYSTEM_INDEXOUTOFRANGEEXCEPTION]);
		} else if (status >= NB_RESTART && status < NB_EXIT) {
			// the loop's back-edge was about to be taken: run this block again, from where that edge goes, after the yield
			pCurrentMethodState->nativeEntry = status - NB_RESTART;
			pCurOp = blockOp;
			numInst = 1;
			GO_NEXT_CHECK();
		} else {
			// an exit: carry on at its target
			pCurOp = pOps + exitTargets[status - NB_EXIT];
			numInst = budget > 1 ? budget : 2;
			GO_NEXT_CHECK();
		}
#else
		Crash("a native block was run on a target without native blocks");
#endif
	}
JIT_NATIVE_LOOP_end:
	GO_NEXT();


JIT_NATIVE_RESUME_start:
	OPCODE_USE(JIT_NATIVE_RESUME);
	{
		// The end of an island. A native block gave the interpreter one instruction to run (a call, a throw, ...) and it has run it: go on in
		// the block, at the entry that follows that instruction. The block instruction does the rest (the loop form: it takes the entry).
		U32 resumeBlockOfs = (U32)GET_OP();
		U32 resumeEntry = (U32)GET_OP();
		pCurrentMethodState->nativeEntry = resumeEntry;
		pCurOp = pOps + resumeBlockOfs;
	}
JIT_NATIVE_RESUME_end:
	GO_NEXT();
JIT_ENDFILTER_start:
	OPCODE_USE(JIT_ENDFILTER);
	{
		// The filter's verdict is on the stack. Put the filter's frame back as it was when the filter
		// began, return to the frame that threw, and either start unwinding to the handler this filter
		// guards (nonzero) or resume the search at the next clause (zero).
		U32 verdict = POP_U32();
		tFilterCtx *pCtx;
		tMethodState *pFilterFrame;
		U32 clause;

		if (pThread->numFilters == 0) {
			Crash("endfilter outside an exception filter");
		}
		pCtx = &pThread->pFilterCtx[--pThread->numFilters];
		pFilterFrame = pCtx->pFrame;
		clause = pCtx->clause;
		pFilterFrame->ipOffset = pCtx->savedIp;
		pFilterFrame->stackOfs = pCtx->savedStackOfs;
		pThread->pCurrentExceptionObject = pCtx->savedException;
		pThread->pThrowTopState = pCtx->pTopState;
		pThread->searchInFilter = pCtx->outerSearchInFilter;
		pThread->pCurrentMethodState = pCtx->pTopState;
		LOAD_METHOD_STATE();
		if (verdict != 0) {
			pThread->pCatchMethodState = pFilterFrame;
			pThread->pCatchExceptionHandler = &pFilterFrame->pMethod->pJITted->pExceptionHeaders[clause];
			pThread->nextFinallyUnwindStack = 0;
			goto unwindToHandler;
		}
		pThread->pSearchFrame = pFilterFrame;
		pThread->searchNextClause = clause + 1;
		goto searchHandlers;
	}
JIT_ENDFILTER_end:
	GO_NEXT_CHECK();

JIT_LEAVE_start:
	OPCODE_USE(JIT_LEAVE);
	{
		U32 src, next;
		tExceptionHeader *pFinally;

		// Which finally clauses cover this leave depends on where we are NOW. ipOffset is only saved at calls,
		// so without this it was stale: a try body that contained no call never ran its finally block.
		pCurrentMethodState->ipOffset = (U32)(pCurOp - pOps);
		src = pCurrentMethodState->ipOffset - 1;
		POP_ALL();
		ofs = GET_OP();
		pFinally = FindLeaveFinally(pJIT, 0, src, ofs, &next);
		if (pFinally != NULL) {
			// Jump to 'finally' section; END_FINALLY carries on to the next one, or to the target
			pCurOp = pOps + pFinally->handlerStart;
			pCurrentMethodState->pOpEndFinally = pOps + ofs;
			pCurrentMethodState->leaveSrc = src;
			pCurrentMethodState->leaveNext = next;
		} else {
			// just branch
			pCurOp = pOps + ofs;
		}
	}
JIT_LEAVE_end:
	GO_NEXT_CHECK();

JIT_END_FINALLY_start:
	OPCODE_USE(JIT_END_FINALLY);
	if (pThread->nextFinallyUnwindStack > 0) {
		// unwinding stack, so jump back to unwind code
		goto finallyUnwindStack;
	} else {
		// Empty the evaluation stack. If the leave exits more try blocks with finally clauses, run the next one;
		// otherwise jump to the instruction the leave named.
		U32 next;
		tExceptionHeader *pNext;
		POP_ALL();
		pNext = FindLeaveFinally(pJIT, pCurrentMethodState->leaveNext, pCurrentMethodState->leaveSrc,
			(U32)(pCurrentMethodState->pOpEndFinally - pOps), &next);
		if (pNext != NULL) {
			pCurrentMethodState->leaveNext = next;
			pCurOp = pOps + pNext->handlerStart;
		} else {
			pCurOp = pCurrentMethodState->pOpEndFinally;
		}
	}
JIT_END_FINALLY_end:
	GO_NEXT_CHECK();

done:
	SAVE_METHOD_STATE();

	return THREAD_STATUS_RUNNING;
}

void JIT_Execute_Init() {
	// Initialise the JIT code addresses
	JIT_Execute(NULL, 0);
}