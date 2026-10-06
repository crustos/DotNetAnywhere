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
#include "WasmJIT.h"
#include "NativeBlocks.h"

#include "JIT_OpCodes.h"
#include "System.Runtime.InteropServices.Marshal.h"
#include "CIL_OpCodes.h"
#include "CLIFile.h"

#include "MetaData.h"
#include "Types.h"
#include "Type.h"
#include "InternalCall.h"
#include "Heap.h"
#include "PInvoke.h"
#include "FFI.h"
#include "VStack.h"

#define CorILMethod_TinyFormat 0x02
#define CorILMethod_MoreSects 0x08

#define CorILMethod_Sect_EHTable 0x01
#define CorILMethod_Sect_FatFormat 0x40
#define CorILMethod_Sect_MoreSects 0x80

#define DYNAMIC_OK 0x100
#define DYNAMIC_JUMP_TARGET 0x200
#define DYNAMIC_EX_START 0x400
#define DYNAMIC_EX_END 0x800
#define DYNAMIC_BYTE_COUNT_MASK 0xff

typedef struct tOps_ tOps;
struct tOps_ {
	tOpWord *p;
	I32 *pSequencePoints;
	U32 capacity;
	U32 ofs;
};

typedef struct tTypeStack_ tTypeStack;
struct tTypeStack_ {
	tMD_TypeDef **ppTypes;
	U32 ofs;
	U32 maxBytes; // The max size of the stack in bytes
};

#define InitOps(ops_, initialCapacity) ops_.capacity = initialCapacity; ops_.ofs = 0; ops_.p = malloc((initialCapacity) * sizeof(tOpWord)); ops_.pSequencePoints = malloc((initialCapacity) * sizeof(I32));
#define DeleteOps(ops_) free(ops_.p); free(ops_.pSequencePoints)

// Turn this into a MACRO at some point?
static tOpWord Translate(U32 op, U32 getDynamic) {
	if (op >= JIT_OPCODE_MAXNUM) {
		Crash("Illegal opcode: %d", op);
	}
	if (jitCodeInfo[op].pEnd == NULL) {
		Crash("Opcode not available: 0x%08x", op);
	}
	if (getDynamic) {
		return (tOpWord)jitCodeInfo[op].isDynamic;
	} else {
		return (tOpWord)jitCodeInfo[op].pStart;
	}
}

#ifdef GEN_COMBINED_OPCODES
#define PushU32(v) PushU32_(&ops, (U32)(v)); PushU32_(&isDynamic, 0)
#define PushI32(v) PushU32_(&ops, (U32)(v)); PushU32_(&isDynamic, 0)
#define PushFloat(v) convFloat.f=(float)(v); PushU32_(&ops, convFloat.u32); PushU32_(&isDynamic, 0)
#define PushDouble(v) convDouble.d=(double)(v); PushU32_(&ops, convDouble.u32.a); PushU32_(&ops, convDouble.u32.b); PushU32_(&isDynamic, 0); PushU32_(&isDynamic, 0)
#define PushPTR(ptr) PushU32_(&ops, (U32)(ptr)); PushU32_(&isDynamic, 0)
#define PushOp(op) PushU32_(&ops, Translate((U32)(op), 0)); PushU32_(&isDynamic,	 Translate((U32)(op), 1))
#define PushOpParam(op, param) PushOp(op); PushU32_(&ops, (U32)(param)); PushU32_(&isDynamic, 0)
#else
#define PushU32(v) PushU32_(&ops, (U32)(v), -1)
#define PushI32(v) PushU32_(&ops, (U32)(v), -1)
#define PushFloat(v) convFloat.f=(float)(v); PushU32_(&ops, convFloat.u32, -1)
#define PushDouble(v) convDouble.d=(double)(v); PushU64(((U64)convDouble.u32.b << 32) | convDouble.u32.a)
#define PushPTR(ptr) PushU32_(&ops, (tOpWord)(ptr), -1)
// A 64-bit constant: one op word on a 64-bit target, otherwise two (low half first, which the interpreter
// reads back as a single U64 in memory order).
#define PushU64(v) PushU64_(&ops, (U64)(v))
#define PushOp(op) PushU32_(&ops, Translate((U32)(op), 0), nextOpSequencePoint)
#define PushOpParam(op, param) PushOp(op); PushU32_(&ops, (U32)(param), -1)
#endif

#define PushBranch() PushU32_(&branchOffsets, ops.ofs, -1)

// An array index, or the length of `newarr`, that is pointer-wide: C# puts a `conv.u` / `conv.i` before the element opcode for a uint, long or ulong, and
// on a 64-bit target the result is 8 bytes on the evaluation stack, where the element operations (and newarr) take 4 -- the stack was left misaligned.
// The index is narrowed here, where the type stack still says what it is: on top by an ordinary conversion, under a stelem's value by
// JIT_NARROW_INDEX_BELOW.  (A 32-bit target has no wide index, and nothing is emitted.)
#define NarrowWideIndexTop() \
	do { \
		if (typeStack.ppTypes[typeStack.ofs - 1]->stackSize > 4) { \
			PushOp(JIT_CONV_FROM_U64 + JIT_CONV_OFFSET_I32); PushU32(0); \
		} \
	} while (0)
#define NarrowWideIndexBelowValue() \
	do { \
		if (typeStack.ppTypes[typeStack.ofs - 2]->stackSize > 4) { \
			PushOpParam(JIT_NARROW_INDEX_BELOW, typeStack.ppTypes[typeStack.ofs - 1]->stackSize); \
		} \
	} while (0)

#define PushStackType(type) PushStackType_(&typeStack, type);
#define PopStackType() (typeStack.ppTypes[--typeStack.ofs])
#define PopStackTypeDontCare() typeStack.ofs--
#define PopStackTypeMulti(number) typeStack.ofs -= number

// The element size of an array whose type is `pType`, as the runtime lays it out, or 0 if pType is not known to be an array
static U32 KnownArrayElementSize(tMD_TypeDef *pType) {
	if (pType == NULL || !TYPE_ISARRAY(pType)) {
		return 0;
	}
	MetaData_Fill_TypeDef(pType->pArrayElementType, NULL, NULL);
	return pType->pArrayElementType->arrayElementSize;
}
#define PopStackTypeAll() typeStack.ofs = 0;

#define MayCopyTypeStack() if (u32Value > cilOfs) ppTypeStacks[u32Value] = DeepCopyTypeStack(&typeStack)

static void PushStackType_(tTypeStack *pTypeStack, tMD_TypeDef *pType) {
	U32 i, size;

	MetaData_Fill_TypeDef(pType, NULL, NULL);
	pTypeStack->ppTypes[pTypeStack->ofs++] = pType;
	// Count current stack size in bytes
	size = 0;
	for (i=0; i<pTypeStack->ofs; i++) {
		size += pTypeStack->ppTypes[i]->stackSize;
	}
	if (size > pTypeStack->maxBytes) {
		pTypeStack->maxBytes = size;
	}
	//printf("Stack ofs = %d; Max stack size: %d (0x%x)\n", pTypeStack->ofs, size, size);
}

static void PushU32_(tOps *pOps, tOpWord v, I32 opSequencePoint) {
	if (pOps->ofs >= pOps->capacity) {
		pOps->capacity <<= 1;
//		printf("a.pOps->p = 0x%08x size=%d\n", pOps->p, pOps->capacity * sizeof(U32));
		pOps->p = realloc(pOps->p, pOps->capacity * sizeof(tOpWord));
		pOps->pSequencePoints = realloc(pOps->pSequencePoints, pOps->capacity * sizeof(U32));
	}
	pOps->pSequencePoints[pOps->ofs] = opSequencePoint;
	pOps->p[pOps->ofs++] = v;
}

#ifndef GEN_COMBINED_OPCODES
static void PushU64_(tOps *pOps, U64 v) {
	if (sizeof(tOpWord) >= sizeof(U64)) {
		PushU32_(pOps, (tOpWord)v, -1);
	} else {
		PushU32_(pOps, (tOpWord)(U32)v, -1);
		PushU32_(pOps, (tOpWord)(U32)(v >> 32), -1);
	}
}
#endif

static U32 GetUnalignedU32(U8 *pCIL, U32 *pCILOfs) {
	U32 a,b,c,d;
	a = pCIL[(*pCILOfs)++];
	b = pCIL[(*pCILOfs)++];
	c = pCIL[(*pCILOfs)++];
	d = pCIL[(*pCILOfs)++];
	return a | (b << 8) | (c << 16) | (d << 24);
}

static tTypeStack* DeepCopyTypeStack(tTypeStack *pToCopy) {
	tTypeStack *pCopy;

	pCopy = TMALLOC(tTypeStack);
	pCopy->maxBytes = pToCopy->maxBytes;
	pCopy->ofs = pToCopy->ofs;
	if (pToCopy->ofs > 0) {
		pCopy->ppTypes = malloc(pToCopy->ofs * sizeof(tMD_TypeDef*));
		memcpy(pCopy->ppTypes, pToCopy->ppTypes, pToCopy->ofs * sizeof(tMD_TypeDef*));
	} else {
		pCopy->ppTypes = NULL;
	}
	return pCopy;
}

static void RestoreTypeStack(tTypeStack *pMainStack, tTypeStack *pCopyFrom) {
	// This does not effect maxBytes, as the current value will always be equal
	// or greater than the value being copied from.
	if (pCopyFrom == NULL) {
		pMainStack->ofs = 0;
	} else {
		pMainStack->ofs = pCopyFrom->ofs;
		if (pCopyFrom->ppTypes != NULL) {
			memcpy(pMainStack->ppTypes, pCopyFrom->ppTypes, pCopyFrom->ofs * sizeof(tMD_TypeDef*));
		}
	}
}

#ifdef GEN_COMBINED_OPCODES
static U32 FindOpCode(void *pAddr) {
	U32 i;
	for (i=0; i<JIT_OPCODE_MAXNUM; i++) {
		if (jitCodeInfo[i].pStart == pAddr) {
			return i;
		}
	}
	Crash("Cannot find opcode for address: 0x%08x", (U32)pAddr);
	FAKE_RETURN;
}

static U32 combinedMemSize = 0;
static U32 GenCombined(tOps *pOps, tOps *pIsDynamic, U32 startOfs, U32 count, U32 *pCombinedSize, void **ppMem) {
	U32 memSize;
	U32 ofs;
	void *pCombined;
	U32 opCopyToOfs;
	U32 shrinkOpsBy;
	U32 goNextSize = (U32)((char*)jitCodeGoNext.pEnd - (char*)jitCodeGoNext.pStart);

	// Get length of final combined code chunk
	memSize = 0;
	for (ofs=0; ofs < count; ofs++) {
		U32 opcode = FindOpCode((void*)pOps->p[startOfs + ofs]);
		U32 size = (U32)((char*)jitCodeInfo[opcode].pEnd - (char*)jitCodeInfo[opcode].pStart);
		memSize += size;
		ofs += (pIsDynamic->p[startOfs + ofs] & DYNAMIC_BYTE_COUNT_MASK) >> 2;
	}
	// Add length of GoNext code
	memSize += goNextSize;

	pCombined = malloc(memSize);
	*ppMem = pCombined;
	combinedMemSize += memSize;
	*pCombinedSize = memSize;
	//log_f(0, "Combined JIT size: %d\n", combinedMemSize);

	// Copy the bits of code into place
	memSize = 0;
	opCopyToOfs = 1;
	for (ofs=0; ofs < count; ofs++) {
		U32 extraOpBytes;
		U32 opcode = FindOpCode((void*)pOps->p[startOfs + ofs]);
		U32 size = (U32)((char*)jitCodeInfo[opcode].pEnd - (char*)jitCodeInfo[opcode].pStart);
		memcpy((char*)pCombined + memSize, jitCodeInfo[opcode].pStart, size);
		memSize += size;
		extraOpBytes = pIsDynamic->p[startOfs + ofs] & DYNAMIC_BYTE_COUNT_MASK;
		memmove(&pOps->p[startOfs + opCopyToOfs], &pOps->p[startOfs + ofs + 1], extraOpBytes);
		opCopyToOfs += extraOpBytes >> 2;
		ofs += extraOpBytes >> 2;
	}
	shrinkOpsBy = ofs - opCopyToOfs;
	// Add GoNext code
	memcpy((char*)pCombined + memSize, jitCodeGoNext.pStart, goNextSize);
	pOps->p[startOfs] = (U32)pCombined;

	return shrinkOpsBy;
}
#endif

static void SetBreakPoint(tMD_MethodDef *pMethodDef, U32 cilOfs, tOps ops)
{
    
}

// Overflow-checked conversions (conv.ovf.*). Returns the destination kind for JIT_CONV_OVF_CHECK
// (the order must match ConvOvfFits in JIT_Execute.c), or -1 if `op` is not one of them.
// *pUnsigned is set for the .un forms, which treat an integer source as unsigned.
// Native int/uint are 32 bits here, like the rest of this JIT.
#define OVF_TO_I1 0
#define OVF_TO_U1 1
#define OVF_TO_I2 2
#define OVF_TO_U2 3
#define OVF_TO_I4 4
#define OVF_TO_U4 5
#define OVF_TO_I8 6
#define OVF_TO_U8 7
static I32 ConvOvfTarget(U32 op, U32 *pUnsigned) {
	*pUnsigned = 0;
	switch (op) {
	case CIL_CONV_OVF_I1_UN: *pUnsigned = 1; /* fall through */
	case CIL_CONV_OVF_I1: return OVF_TO_I1;
	case CIL_CONV_OVF_U1_UN: *pUnsigned = 1; /* fall through */
	case CIL_CONV_OVF_U1: return OVF_TO_U1;
	case CIL_CONV_OVF_I2_UN: *pUnsigned = 1; /* fall through */
	case CIL_CONV_OVF_I2: return OVF_TO_I2;
	case CIL_CONV_OVF_U2_UN: *pUnsigned = 1; /* fall through */
	case CIL_CONV_OVF_U2: return OVF_TO_U2;
	case CIL_CONV_OVF_I4_UN: *pUnsigned = 1; /* fall through */
	case CIL_CONV_OVF_I4: return OVF_TO_I4;
	// a native int is as wide as a pointer: 32 bits on a 32-bit target, 64 on a 64-bit one
	case CIL_CONV_OVF_I_UN: *pUnsigned = 1; /* fall through */
	case CIL_CONV_OVF_I: return (sizeof(void*) > 4) ? OVF_TO_I8 : OVF_TO_I4;
	case CIL_CONV_OVF_U4_UN: *pUnsigned = 1; /* fall through */
	case CIL_CONV_OVF_U4: return OVF_TO_U4;
	case CIL_CONV_OVF_U_UN: *pUnsigned = 1; /* fall through */
	case CIL_CONV_OVF_U: return (sizeof(void*) > 4) ? OVF_TO_U8 : OVF_TO_U4;
	case CIL_CONV_OVF_I8_UN: *pUnsigned = 1; /* fall through */
	case CIL_CONV_OVF_I8: return OVF_TO_I8;
	case CIL_CONV_OVF_U8_UN: *pUnsigned = 1; /* fall through */
	case CIL_CONV_OVF_U8: return OVF_TO_U8;
	}
	return -1;
}

// Does this conversion opcode treat an integer source as unsigned? (ECMA-335: conv.u8, conv.r.un
// and the .un forms of conv.ovf.*; every other conversion treats it as signed.) This is decided
// by the opcode, not by the static type of the value being converted: `(long)(int)uintVar` is a
// bare conv.i8 on a uint local and must sign-extend. Choosing by the local's type made it
// zero-extend, and made (double)(long)ulongVar convert as unsigned.
static int ConvSourceIsUnsigned(U32 op) {
	U32 isUn;
	if (op == CIL_CONV_U8 || op == CIL_CONV_R_UN) {
		return 1;
	}
	return ConvOvfTarget(op, &isUn) >= 0 && isUn;
}


// ---------------------------------------------------------------------------------------------------------------
// Fusing instructions. JITit has translated the method one CIL instruction at a time; this pass, run before the branch
// targets are converted from CIL offsets, replaces short runs that have a fused equivalent (see tools/gen_fused_ops.py:
// `ldloc a; ldloc b; add` becomes one instruction that reads both locals) with that one instruction. A run is never
// fused across anything that could be jumped to or that marks a region boundary, so no jump can land inside one:
//  * every branch target (all of them are in `branchOffsets` by now, backward ones too),
//  * the start and end of every try block, handler and filter,
//  * an instruction that carries a debugger sequence point.
// Compacting shifts every op offset after a fusion, so pJITOffsets, the recorded branch-operand positions and the
// sequence points are all remapped. DNA_NO_FUSION=1 turns the pass off.
// ---------------------------------------------------------------------------------------------------------------
#define FUSED_TABLES
#include "JIT_Fused.gen.h"
#undef FUSED_TABLES

#define NUM_FUSED_BINS ((U32)(sizeof(fusedBins) / sizeof(fusedBins[0])))
#define NUM_FUSED_BCCS ((U32)(sizeof(fusedBccs) / sizeof(fusedBccs[0])))

// Instructions that a native block does not compile but gives to the interpreter, which runs the one instruction and hands back (an
// "island": the block leaves by an exit to a stub [the instruction][JIT_NATIVE_RESUME block entry], and is entered again after it). Any
// instruction could be, because the evaluation stack is in memory; these are the ones that do not use the frame's own offsets or the
// op stream's positions (so they can be copied as they are). Every alias of a handler is listed (see stencil_alias_groups).
#define MAX_ISLAND_OPS 64
static const U32 islandOps[] = {
	JIT_CALL_O, JIT_CALL_PTR, JIT_CALLVIRT_O, JIT_CALL_INTERFACE, JIT_THROW, JIT_NEWOBJECT, JIT_NEWOBJECT_VALUETYPE, JIT_FFI_CALL,
	JIT_CAST_CLASS, JIT_IS_INSTANCE, JIT_UNBOX2OBJECT, JIT_UNBOX2VALUETYPE, JIT_LOAD_STRING,
	JIT_BOX_INT64, JIT_BOX_INT32, JIT_BOX_INTNATIVE, JIT_BOX_F32, JIT_BOX_PTR, JIT_BOX_O, JIT_BOX_F64, JIT_BOX_VALUETYPE,
	JIT_LOADSTATICFIELD_CHECKTYPEINIT_INT64, JIT_LOADSTATICFIELD_CHECKTYPEINIT_INT32, JIT_LOADSTATICFIELD_CHECKTYPEINIT_INTNATIVE,
	JIT_LOADSTATICFIELD_CHECKTYPEINIT_F32, JIT_LOADSTATICFIELD_CHECKTYPEINIT_PTR, JIT_LOADSTATICFIELD_CHECKTYPEINIT_O,
	JIT_LOADSTATICFIELD_CHECKTYPEINIT_F64, JIT_LOADSTATICFIELD_CHECKTYPEINIT_VALUETYPE,
	JIT_LOADSTATICFIELD_INT64, JIT_LOADSTATICFIELD_INT32, JIT_LOADSTATICFIELD_INTNATIVE, JIT_LOADSTATICFIELD_F32,
	JIT_LOADSTATICFIELD_PTR, JIT_LOADSTATICFIELD_O, JIT_LOADSTATICFIELD_F64,
	JIT_STORESTATICFIELD_INT64, JIT_STORESTATICFIELD_INT32, JIT_STORESTATICFIELD_INTNATIVE, JIT_STORESTATICFIELD_F32,
	JIT_STORESTATICFIELD_F64, JIT_STORESTATICFIELD_PTR, JIT_STORESTATICFIELD_O, JIT_STORESTATICFIELD_VALUETYPE };
#define NUM_ISLAND_OPS ((int)(sizeof(islandOps) / sizeof(islandOps[0])))

// Instructions with no operand that are one stencil each (long and double arithmetic, the conversions that change the size),
// in the order of simpleStencils in StencilIdFor
#define NUM_SIMPLE_OPS 26
static const U32 simpleOps[NUM_SIMPLE_OPS] = {
	JIT_ADD_I64I64, JIT_SUB_I64I64, JIT_MUL_I64I64, JIT_AND_I64I64, JIT_OR_I64I64, JIT_XOR_I64I64, JIT_SHL_I64, JIT_SHR_I64,
	JIT_SHR_UN_I64, JIT_NEG_I64, JIT_ADD_F64F64, JIT_SUB_F64F64, JIT_MUL_F64F64, JIT_DIV_F64F64, JIT_NEG_F64,
	JIT_CONV_I32_I64, JIT_CONV_U32_I64, JIT_CONV_I32_R64, JIT_CONV_R32_R64, JIT_CONV_R64_R32,
	JIT_CONV_I32_U64, JIT_CONV_U32_U64, JIT_CONV_I64_R64, JIT_CONV_I64_R32, JIT_CONV_R64_I64, JIT_CONV_R32_I64 };      // (these two are cvtul as well: one handler body, but not one address)
// The long and double compare-and-branches, in the order of the stencils that do them
static const U32 lbccOps[6] = { JIT_BEQ_I64I64, JIT_BGE_I64I64, JIT_BGT_I64I64, JIT_BLE_I64I64, JIT_BLT_I64I64, JIT_BNE_UN_I64I64 };
static const U32 dbccOps[10] = { JIT_BEQ_F64F64, JIT_BNE_UN_F64F64, JIT_BLT_F64F64, JIT_BLE_F64F64, JIT_BGT_F64F64,
	JIT_BGE_F64F64, JIT_BLT_UN_F64F64, JIT_BLE_UN_F64F64, JIT_BGT_UN_F64F64, JIT_BGE_UN_F64F64 };

// The float32 compare-and-branch instructions, in the order of the stencils that do them (see StencilIdFor)
static const U32 fbccOps[10] = { JIT_BEQ_F32F32, JIT_BNE_UN_F32F32, JIT_BLT_F32F32, JIT_BLE_F32F32, JIT_BGT_F32F32,
	JIT_BGE_F32F32, JIT_BLT_UN_F32F32, JIT_BLE_UN_F32F32, JIT_BGT_UN_F32F32, JIT_BGE_UN_F32F32 };

enum { K_OTHER = 0, K_LOADL, K_CONSTI, K_CONSTF, K_STOREL, K_BIN, K_BCC, K_FNEG,
	K_LDA, K_LDP, K_STP, K_DUP4, K_DUP8, K_LDFLD4, K_STFLD4, K_BR, K_BRT, K_BRF, K_FBCC, K_CVTIF, K_CVTFI,
	K_LDELEM4, K_LDELEMU1, K_LDELEMB4, K_STELEM4, K_STELEM1, K_LDELEMA, K_LDLEN, K_CVTII, K_CVTMASK,
	K_SIMPLE, K_LDC8, K_CVTLI, K_CVTDI, K_LBCC, K_DBCC,
	K_CALLD, K_CALLVIRTD, K_RET, K_LDFLD8, K_STFLD8, K_LDFLDA, K_ISLAND, K_BRT8, K_BRF8, K_FFI };

static struct {
	int ready;
	tOpWord loadSlot[8], storeSlot[8], constI[4];
	tOpWord load32a, load32b, store32a, store32b, loadI32, loadF32;
	tOpWord bin[sizeof(fusedBins) / sizeof(fusedBins[0])];
	tOpWord bcc[sizeof(fusedBccs) / sizeof(fusedBccs[0])];
	tOpWord inc, load2, negF32;
	tOpWord lda, ldp[5], stp[5], dup4, dup8, ldfld4, stfld4[2], br, brt, brf, cvtif, cvtfi;
	tOpWord fbcc[10];
	tOpWord simple[NUM_SIMPLE_OPS], lbcc[6], dbcc[10], ldc8[2], cvtli[2], cvtdi;
	tOpWord callO, callPtr, callvirtO, ret, ldfld8, stfld8[5], ldflda, loadString, loadStringMd, brt8, brf8, ffiCall;
	tOpWord island[MAX_ISLAND_OPS];
	tOpWord ldelem4[3], ldelemu1, ldelemb4, stelem4, stelem1, ldelema, ldlen, cvtii[2], cvtmask[2];
	U32 addIdx, subIdx;
} fc;

static void InitFuseCodes(void) {
	U32 i;
	for (i = 0; i < 8; i++) {
		fc.loadSlot[i] = Translate(JIT_LOADPARAMLOCAL_0 + i, 0);
		fc.storeSlot[i] = Translate(JIT_STOREPARAMLOCAL_0 + i, 0);
	}
	for (i = 0; i < 4; i++) {
		fc.constI[i] = Translate(JIT_LOAD_I4_M1 + i, 0);
	}
	fc.load32a = Translate(JIT_LOADPARAMLOCAL_INT32, 0);   // INT32 and F32 share one handler: both copy 4 bytes
	fc.load32b = Translate(JIT_LOADPARAMLOCAL_F32, 0);
	fc.store32a = Translate(JIT_STOREPARAMLOCAL_INT32, 0);
	fc.store32b = Translate(JIT_STOREPARAMLOCAL_F32, 0);
	fc.loadI32 = Translate(JIT_LOAD_I32, 0);
	fc.loadF32 = Translate(JIT_LOAD_F32, 0);
	fc.addIdx = fc.subIdx = 0xffffffff;
	for (i = 0; i < NUM_FUSED_BINS; i++) {
		fc.bin[i] = Translate(fusedBins[i].op, 0);
		if (fusedBins[i].op == FUSED_ADD_OP) { fc.addIdx = i; }
		if (fusedBins[i].op == FUSED_SUB_OP) { fc.subIdx = i; }
	}
	for (i = 0; i < NUM_FUSED_BCCS; i++) {
		fc.bcc[i] = Translate(fusedBccs[i].op, 0);
	}
	fc.inc = Translate(JIT_FUSED_INC_L, 0);
	fc.load2 = Translate(JIT_FUSED_LOAD2, 0);
	fc.negF32 = Translate(JIT_NEG_F32, 0);
	fc.lda = Translate(JIT_LOAD_PARAMLOCAL_ADDR, 0);
	fc.ldp[0] = Translate(JIT_LOADPARAMLOCAL_O, 0);          // (these share one handler, but do not rely on it)
	fc.ldp[1] = Translate(JIT_LOADPARAMLOCAL_PTR, 0);
	fc.ldp[2] = Translate(JIT_LOADPARAMLOCAL_INTNATIVE, 0);
	fc.stp[0] = Translate(JIT_STOREPARAMLOCAL_O, 0);
	fc.stp[1] = Translate(JIT_STOREPARAMLOCAL_PTR, 0);
	fc.stp[2] = Translate(JIT_STOREPARAMLOCAL_INTNATIVE, 0);
	fc.dup4 = Translate(JIT_DUP_4, 0);
	fc.dup8 = Translate(JIT_DUP_8, 0);
	fc.ldfld4 = Translate(JIT_LOADFIELD_4, 0);
	fc.br = Translate(JIT_BRANCH, 0);
	for (i = 0; i < 10; i++) { fc.fbcc[i] = Translate(fbccOps[i], 0); }
	fc.cvtif = Translate(JIT_CONV_I32_R32, 0);
	for (i = 0; i < NUM_SIMPLE_OPS; i++) { fc.simple[i] = Translate(simpleOps[i], 0); }
	for (i = 0; i < 6; i++) { fc.lbcc[i] = Translate(lbccOps[i], 0); }
	for (i = 0; i < 10; i++) { fc.dbcc[i] = Translate(dbccOps[i], 0); }
	// Instructions that share a handler body do NOT share an address (the labels are separate), so every one the JIT can emit
	// for a stencil's meaning is compared, not just one of them
	fc.ldc8[0] = Translate(JIT_LOAD_I64, 0);
	fc.ldc8[1] = Translate(JIT_LOAD_F64, 0);
	fc.cvtli[0] = Translate(JIT_CONV_I64_I32, 0);
	fc.cvtli[1] = Translate(JIT_CONV_U64_I32, 0);
	fc.cvtdi = Translate(JIT_CONV_R64_I32, 0);
	fc.callO = Translate(JIT_CALL_O, 0);
	fc.callPtr = Translate(JIT_CALL_PTR, 0);
	fc.callvirtO = Translate(JIT_CALLVIRT_O, 0);
	fc.ret = Translate(JIT_RETURN, 0);
	fc.ldfld8 = Translate(JIT_LOADFIELD, 0);                  // the general field load: used here only for 8-byte fields
	fc.stfld8[0] = Translate(JIT_STOREFIELD_INT64, 0);
	fc.stfld8[1] = Translate(JIT_STOREFIELD_F64, 0);
	fc.stfld8[2] = Translate(JIT_STOREFIELD_O, 0);
	fc.stfld8[3] = Translate(JIT_STOREFIELD_INTNATIVE, 0);
	fc.stfld8[4] = Translate(JIT_STOREFIELD_PTR, 0);
	fc.ldflda = Translate(JIT_LOAD_FIELD_ADDR, 0);
	fc.loadString = Translate(JIT_LOAD_STRING, 0);
	fc.ffiCall = Translate(JIT_FFI_CALL, 0);
	fc.brt8 = Translate(JIT_BRANCH_TRUE_PTR, 0);
	fc.brf8 = Translate(JIT_BRANCH_FALSE_PTR, 0);
	fc.loadStringMd = Translate(JIT_LOAD_STRING_MD, 0);
	for (i = 0; i < MAX_ISLAND_OPS; i++) { fc.island[i] = 0; }
	for (i = 0; i < (U32)NUM_ISLAND_OPS; i++) {
		// (a handler that does not exist in this build is simply not an island)
		fc.island[i] = (islandOps[i] < JIT_OPCODE_MAXNUM && jitCodeInfo[islandOps[i]].pEnd != NULL) ? Translate(islandOps[i], 0) : 0;
	}
	fc.ldp[3] = Translate(JIT_LOADPARAMLOCAL_INT64, 0);    // a long and a double are 8-byte copies, as a reference is
	fc.ldp[4] = Translate(JIT_LOADPARAMLOCAL_F64, 0);
	fc.stp[3] = Translate(JIT_STOREPARAMLOCAL_INT64, 0);
	fc.stp[4] = Translate(JIT_STOREPARAMLOCAL_F64, 0);
	fc.ldelem4[0] = Translate(JIT_LOAD_ELEMENT_I32, 0);     // .i4, .u4 and .r4: all load 4 bytes, but they are three instructions
	fc.ldelem4[1] = Translate(JIT_LOAD_ELEMENT_U32, 0);     // (their handlers are one piece of code, yet ldelem.r4 did not match
	fc.ldelem4[2] = Translate(JIT_LOAD_ELEMENT_R32, 0);     // the I32 address, so each is compared)
	fc.ldelemu1 = Translate(JIT_LOAD_ELEMENT_U8_1, 0);        // (not JIT_LOAD_ELEMENT_U8: that one looks at the array at run time)
	fc.ldelemb4 = Translate(JIT_LOAD_ELEMENT_U8_4, 0);
	fc.stelem4 = Translate(JIT_STORE_ELEMENT_I4, 0);
	fc.stelem1 = Translate(JIT_STORE_ELEMENT_I1, 0);
	fc.ldelema = Translate(JIT_LOAD_ELEMENT_ADDR_N, 0);
	fc.ldlen = Translate(JIT_LOAD_VECTOR_LEN, 0);
	fc.cvtii[0] = Translate(JIT_CONV_I32_I32, 0);
	fc.cvtii[1] = Translate(JIT_CONV_U32_I32, 0);
	fc.cvtmask[0] = Translate(JIT_CONV_I32_U32, 0);
	fc.cvtmask[1] = Translate(JIT_CONV_U32_U32, 0);
	fc.cvtfi = Translate(JIT_CONV_R32_I32, 0);
	fc.brt = Translate(JIT_BRANCH_TRUE, 0);
	fc.brf = Translate(JIT_BRANCH_FALSE, 0);
	fc.stfld4[0] = Translate(JIT_STOREFIELD_INT32, 0);
	fc.stfld4[1] = Translate(JIT_STOREFIELD_F32, 0);
	fc.ready = 1;
}

// What a CIL instruction was translated to, judged by the address of its first op and its length in words
// The stencils that put the address of a C function in r11 (generated with the FFI stencils: a build with no manifest has none)
static int ffiStFnLo = -2, ffiStFnHi = -2;
static int FfiStencilIds(void) {
	if (ffiStFnLo == -2) {
		ffiStFnLo = NativeBlock_FindStencil("fnlo");
		ffiStFnHi = NativeBlock_FindStencil("fnhi");
	}
	return ffiStFnLo >= 0 && ffiStFnHi >= 0;
}

static int ClassifyOps(const tOpWord *p, U32 len, U32 *pVal, U32 *pIdx) {
	U32 i;
	if (p[0] == fc.callO || p[0] == fc.callPtr) { return K_CALLD; }       // (the callee is the operand, read where it is used)
	if (p[0] == fc.callvirtO) { return K_CALLVIRTD; }
	if (p[0] == fc.ffiCall && len == 2 && FfiStencilIds()) {
		// a call of a C function from the FFI manifest, if there is a stencil that makes it (else it is an island, below)
		const tFFIEntry *pFfi = (const tFFIEntry*)p[1];
		if (pFfi->stencil != NULL && NativeBlock_FindStencil(pFfi->stencil) >= 0) { return K_FFI; }
	}
	for (i = 0; i < (U32)NUM_ISLAND_OPS; i++) {
		if (fc.island[i] != 0 && p[0] == fc.island[i]) { return K_ISLAND; }
	}
	if (len == 1) {
		for (i = 0; i < 8; i++) {
			if (p[0] == fc.loadSlot[i]) { *pVal = i * 4; return K_LOADL; }
			if (p[0] == fc.storeSlot[i]) { *pVal = i * 4; return K_STOREL; }
		}
		for (i = 0; i < 4; i++) {
			if (p[0] == fc.constI[i]) { *pVal = (U32)((I32)i - 1); return K_CONSTI; }
		}
		for (i = 0; i < NUM_FUSED_BINS; i++) {
			if (p[0] == fc.bin[i]) { *pIdx = i; return K_BIN; }
		}
		if (p[0] == fc.negF32) { return K_FNEG; }
		if (p[0] == fc.dup4) { return K_DUP4; }
		if (p[0] == fc.dup8) { return K_DUP8; }
		if (p[0] == fc.cvtif) { return K_CVTIF; }
		if (p[0] == fc.ret) { return K_RET; }
		for (i = 0; i < NUM_SIMPLE_OPS; i++) {
			if (p[0] == fc.simple[i]) { *pIdx = i; return K_SIMPLE; }
		}
		if (p[0] == fc.ldelem4[0] || p[0] == fc.ldelem4[1] || p[0] == fc.ldelem4[2]) { return K_LDELEM4; }
		if (p[0] == fc.ldelemu1) { return K_LDELEMU1; }
		if (p[0] == fc.ldelemb4) { return K_LDELEMB4; }
		if (p[0] == fc.stelem4) { return K_STELEM4; }
		if (p[0] == fc.stelem1) { return K_STELEM1; }
		if (p[0] == fc.ldlen) { return K_LDLEN; }
	} else if (len == 2) {
		if (p[0] == fc.load32a || p[0] == fc.load32b) { *pVal = (U32)p[1]; return K_LOADL; }
		if (p[0] == fc.store32a || p[0] == fc.store32b) { *pVal = (U32)p[1]; return K_STOREL; }
		if (p[0] == fc.loadI32) { *pVal = (U32)p[1]; return K_CONSTI; }
		if (p[0] == fc.loadF32) { *pVal = (U32)p[1]; return K_CONSTF; }
		if (p[0] == fc.lda) { *pVal = (U32)p[1]; return K_LDA; }
		for (i = 0; i < 5; i++) {
			if (p[0] == fc.ldp[i]) { *pVal = (U32)p[1]; return K_LDP; }
			if (p[0] == fc.stp[i]) { *pVal = (U32)p[1]; return K_STP; }
		}
		if (p[0] == fc.ldc8[0] || p[0] == fc.ldc8[1]) { return K_LDC8; }
		if (p[0] == fc.callO || p[0] == fc.callPtr) { return K_CALLD; }       // the callee is the operand (read where it is used)
		if (p[0] == fc.callvirtO) { return K_CALLVIRTD; }                            // the 64 bits are the operand word (read in EmitBlock)
		if (p[0] == fc.cvtli[0] || p[0] == fc.cvtli[1]) { *pVal = (U32)p[1]; return K_CVTLI; }       // val: the narrowing shift
		if (p[0] == fc.cvtdi) { *pVal = (U32)p[1]; return K_CVTDI; }
		for (i = 0; i < 6; i++) {
			if (p[0] == fc.lbcc[i]) { *pIdx = i; *pVal = (U32)p[1]; return K_LBCC; }
		}
		for (i = 0; i < 10; i++) {
			if (p[0] == fc.dbcc[i]) { *pIdx = i; *pVal = (U32)p[1]; return K_DBCC; }
		}
		if (p[0] == fc.ldfld4) { *pVal = (U32)p[1]; return K_LDFLD4; }
		if (p[0] == fc.ldflda) { *pVal = (U32)p[1]; return K_LDFLDA; }
		if (p[0] == fc.ldfld8) {
			// the general load: only an 8-byte field is done here, and its offset is only known once its type has been filled in
			tMD_FieldDef *pField = (tMD_FieldDef*)p[1];
			if (pField != NULL && pField->pParentType != NULL && pField->pParentType->isFilled && pField->memSize == 8) {
				*pVal = pField->memOffset;
				return K_LDFLD8;
			}
			return K_OTHER;
		}
		for (i = 0; i < 5; i++) {
			if (p[0] == fc.stfld8[i]) {
				tMD_FieldDef *pField = (tMD_FieldDef*)p[1];
				if (pField != NULL && pField->pParentType != NULL && pField->pParentType->isFilled) {
					*pVal = pField->memOffset;
					return K_STFLD8;
				}
				return K_OTHER;
			}
		}
		if (p[0] == fc.stfld4[0] || p[0] == fc.stfld4[1]) {
			// the operand is the field; its offset is only known once its type has been filled in
			tMD_FieldDef *pField = (tMD_FieldDef*)p[1];
			if (pField != NULL && pField->pParentType != NULL && pField->pParentType->isFilled) {
				*pVal = pField->memOffset;
				return K_STFLD4;
			}
			return K_OTHER;
		}
		if (p[0] == fc.cvtfi) { *pVal = (U32)p[1]; return K_CVTFI; }       // val: the narrowing shift
		if (p[0] == fc.ldelema) { *pVal = (U32)p[1]; return K_LDELEMA; }   // val: the element size
		if (p[0] == fc.cvtii[0] || p[0] == fc.cvtii[1]) { *pVal = (U32)p[1]; return K_CVTII; }       // val: the shift
		if (p[0] == fc.cvtmask[0] || p[0] == fc.cvtmask[1]) { *pVal = (U32)p[1]; return K_CVTMASK; }   // val: the mask
		for (i = 0; i < 10; i++) {
			if (p[0] == fc.fbcc[i]) { *pIdx = i; *pVal = (U32)p[1]; return K_FBCC; }
		}
		if (p[0] == fc.br) { *pVal = (U32)p[1]; return K_BR; }
		if (p[0] == fc.brt) { *pVal = (U32)p[1]; return K_BRT; }
		if (p[0] == fc.brt8) { *pVal = (U32)p[1]; return K_BRT8; }
		if (p[0] == fc.brf8) { *pVal = (U32)p[1]; return K_BRF8; }
		if (p[0] == fc.brf) { *pVal = (U32)p[1]; return K_BRF; }
		for (i = 0; i < NUM_FUSED_BCCS; i++) {
			if (p[0] == fc.bcc[i]) { *pIdx = i; *pVal = (U32)p[1]; return K_BCC; }    // val: the target, a CIL offset
		}
	}
	return K_OTHER;
}

#if NATIVE_BLOCKS
// Native blocks: which stencil does an instruction have (or -1), and how long a run of them may a block be.
// A block costs a call out of the interpreter and back, so a short straight run is better left to the interpreter and its
// fused instructions; but a run that contains a loop pays that once for all its iterations, so it is worth a much shorter one.
#define BLOCK_MIN_LOOP 6
#define BLOCK_MAX 2048
static U32 BlockMinStraight(void) {
	static int v = -1;
	if (v < 0) { v = getenv("DNA_BLOCK_MIN") != NULL ? atoi(getenv("DNA_BLOCK_MIN")) : 8; }
	return (U32)v;
}
static int StencilIdFor(int kind, U32 idx) {
	switch (kind) {
	case K_LOADL: return ST_LDL;
	case K_STOREL: return ST_STL;
	case K_CONSTI:
	case K_CONSTF: return ST_LDC;
	case K_FNEG: return ST_FNEG;
	case K_LDA: return ST_LDA;
	case K_LDP: return ST_LDP;
	case K_STP: return ST_STP;
	case K_DUP4: return ST_DUP4;
	case K_DUP8: return ST_DUP8;
	case K_LDFLD4: return ST_LDFLD4;
	case K_STFLD4: return ST_STFLD4;
	case K_LDFLD8: return ST_LDFLD8;
	case K_STFLD8: return ST_STFLD8;
	case K_LDFLDA: return ST_LDFLDA;
	case K_FBCC: {
		static const int fbccStencils[10] = { ST_JFEQ, ST_JFNE, ST_JFLT, ST_JFLE, ST_JFGT, ST_JFGE, ST_JFLT_UN, ST_JFLE_UN, ST_JFGT_UN, ST_JFGE_UN };
		return fbccStencils[idx];
	}
	case K_CVTIF: return ST_CVTIF;
	case K_SIMPLE: {
		static const int simpleStencils[NUM_SIMPLE_OPS] = { ST_LADD, ST_LSUB, ST_LMUL, ST_LAND, ST_LOR, ST_LXOR, ST_LSHL, ST_LSHR,
			ST_LSHRUN, ST_LNEG, ST_DADD, ST_DSUB, ST_DMUL, ST_DDIV, ST_DNEG, ST_CVTIL, ST_CVTUL, ST_CVTID, ST_CVTFD, ST_CVTDF,
			ST_CVTUL, ST_CVTUL, ST_CVTLD, ST_CVTLF, ST_CVTDL, ST_CVTFL };
		return simpleStencils[idx];
	}
	case K_FFI: return ST_LDL;           // (a placeholder: EmitBlock makes three stencils of it)
	case K_ISLAND: return ST_J;          // an island leaves the block by an exit: an unconditional jump to it
	case K_CALLD:
	case K_CALLVIRTD: return ST_LDL;     // (a placeholder: only calls with a recipe keep this kind; EmitBlock splices the callee in)
	case K_LDC8: return ST_LDC8LO;       // (EmitBlock makes it a pair: the low half, then the high half)
	case K_CVTLI: return ST_CVTLI;
	case K_CVTDI: return ST_CVTDI;
	case K_LBCC: {
		static const int lbccStencils[6] = { ST_LBEQ, ST_LBGE, ST_LBGT, ST_LBLE, ST_LBLT, ST_LBNE };
		return lbccStencils[idx];
	}
	case K_DBCC: {
		static const int dbccStencils[10] = { ST_JDEQ, ST_JDNE, ST_JDLT, ST_JDLE, ST_JDGT, ST_JDGE, ST_JDLT_UN, ST_JDLE_UN, ST_JDGT_UN, ST_JDGE_UN };
		return dbccStencils[idx];
	}
	case K_LDELEM4: return ST_LDELEM4;
	case K_LDELEMU1: return ST_LDELEMU1;
	case K_LDELEMB4: return ST_LDELEMB4;
	case K_STELEM4: return ST_STELEM4;
	case K_STELEM1: return ST_STELEM1;
	case K_LDELEMA: return ST_LDELEMA;
	case K_LDLEN: return ST_LDLEN;
	case K_CVTII: return ST_CVTII;
	case K_CVTMASK: return ST_CVTMASK;
	case K_CVTFI: return ST_CVTFI;
	case K_BR: return ST_J;
	case K_BRT8: return ST_JT8;
	case K_BRF8: return ST_JF8;
	case K_BRT: return ST_JT;
	case K_BRF: return ST_JF;
	case K_BCC:
		switch (fusedBccs[idx].op) {
		case JIT_BEQ_I32I32: return ST_JEQ;
		case JIT_BGE_I32I32: return ST_JGE;
		case JIT_BGT_I32I32: return ST_JGT;
		case JIT_BLE_I32I32: return ST_JLE;
		case JIT_BLT_I32I32: return ST_JLT;
		case JIT_BNE_UN_I32I32: return ST_JNE;
		}
		return -1;
	case K_BIN:
		switch (fusedBins[idx].op) {
		case JIT_ADD_F32F32: return ST_FADD;
		case JIT_SUB_F32F32: return ST_FSUB;
		case JIT_MUL_F32F32: return ST_FMUL;
		case JIT_DIV_F32F32: return ST_FDIV;
		case JIT_ADD_I32I32: return ST_IADD;
		case JIT_SUB_I32I32: return ST_ISUB;
		case JIT_MUL_I32I32: return ST_IMUL;
		case JIT_AND_I32I32: return ST_IAND;
		case JIT_OR_I32I32: return ST_IOR;
		case JIT_XOR_I32I32: return ST_IXOR;
		case JIT_SHL_I32: return ST_ISHL;
		case JIT_SHR_I32: return ST_ISHR;
		case JIT_SHR_UN_I32: return ST_ISHRUN;
		}
	}
	return -1;
}

// A method whose whole body is one loop-free native block that leaves only by returning can be made into a block again somewhere else:
// these are its stencils, their holes, and where they branch (to each other: -1 where they do not). A caller's block then contains
// the callee instead of calling it (see InlineRecipeFor and EmitBlock).
typedef struct {
	U32 n; unsigned *ids; U32 *holes; int *tgt;
	// The islands in it: which stencil is the exit for each, and the words of the instruction the interpreter runs there (an ldstr has been
	// made one that carries its own metadata). Nothing else leaves a recipe.
	U32 numIslands; U32 *islandAt; tOpWord **islandWords; U32 *islandLen;
	// Some stencil branches to the method's own return, which in a recipe is the end of it: tgt is n, and the caller puts a stencil there
	int hasEnd;
} tBlockRecipe;

// The stencils whose hole is an offset in the frame: the ones that have to move when the callee's frame becomes part of the caller's
static int IsFrameStencil(unsigned id) {
	return id == ST_LDL || id == ST_STL || id == ST_LDP || id == ST_STP || id == ST_LDA || id == ST_ZERO4 || id == ST_ZERO8;
}

#define METHODIMPL_NOINLINING 0x0008
#define MAX_INLINE_STENCILS 160

// The recipe for the method `m` that a block may contain in place of a call to it, or NULL. A call is inlinable if its target is known
// now (a virtual method's is not), the target's whole body is a block with a recipe (so it calls nothing, allocates nothing, has no
// exception handler), and it is not being compiled at this moment (that is recursion). The callee is compiled now if it has not been yet.
// DNA_NO_INLINE=1 turns this off.
static tBlockRecipe* InlineRecipeFor1(tMD_MethodDef *m, int isCallvirt, tMD_MethodDef *caller, int allowIslands, const char **why);
static tBlockRecipe* InlineRecipeFor(tMD_MethodDef *m, int isCallvirt, tMD_MethodDef *caller, int allowIslands) {
	const char *why = "inlined";
	tBlockRecipe *r = InlineRecipeFor1(m, isCallvirt, caller, allowIslands, &why);
	if (getenv("DNA_FUSION_DEBUG") != NULL && m != NULL) {
		fprintf(stderr, "  call to %s.%s: %s\n", m->pParentType != NULL ? m->pParentType->name : "?", m->name, r != NULL ? "INLINED" : why);
	}
	return r;
}
static tBlockRecipe* InlineRecipeFor1(tMD_MethodDef *m, int isCallvirt, tMD_MethodDef *caller, int allowIslands, const char **why) {
	static int disabled = -1;
	tJITted *j;
	if (disabled < 0) { disabled = (getenv("DNA_NO_INLINE") != NULL); }
	if (disabled || !NativeBlock_Enabled() || m == NULL || m == caller || m->pParentType == NULL || !m->isFilled) {
		*why = "inlining is off, or the callee is not ready"; return NULL;
	}
	// A virtual method has one possible target if nothing can override it: it is final (a method that implements an interface is virtual final),
	// or its class is sealed.
	if (isCallvirt && METHOD_ISVIRTUAL(m) && !(m->flags & 0x0020 /* final */) && !(m->pParentType->flags & 0x0100 /* sealed */)) {
		*why = "virtual: the target depends on the object"; return NULL;
	}
	if ((m->implFlags & (METHODIMPLATTRIBUTES_INTERNALCALL | METHODIMPLATTRIBUTES_CODETYPE_MASK | METHODIMPL_NOINLINING)) != 0 ||
			(m->flags & (0x0400 /* abstract */ | METHODATTRIBUTES_PINVOKEIMPL)) != 0) {
		*why = "internal call, native, NoInlining, abstract or P/Invoke"; return NULL;
	}
	if (m->pJITted == NULL) {
		JIT_Prepare(m, 0);
	}
	j = m->pJITted;
	if (j == NULL || j->pOps == NULL) {
		*why = "being compiled (recursion)"; return NULL;
	}
	if (j->pRecipe != NULL) {
		// DNA_INLINE_LIMIT=n: allow only the first n inlines, to find by bisection which one is at fault
		static int limit = -2, serial = 0;
		if (limit == -2) { limit = getenv("DNA_INLINE_LIMIT") != NULL ? atoi(getenv("DNA_INLINE_LIMIT")) : -1; }
		if (limit >= 0 && serial++ >= limit) { *why = "past DNA_INLINE_LIMIT"; return NULL; }
	}
	if (j->pRecipe == NULL || ((tBlockRecipe*)j->pRecipe)->n > MAX_INLINE_STENCILS) {
		*why = j->pRecipe == NULL ? "the body is not one loop-free native block ending in ret" : "too big"; return NULL;
	}
	if (((tBlockRecipe*)j->pRecipe)->numIslands > 0 && !allowIslands) {
		*why = "has islands, and the caller may not (it has exception handlers, or islands are off)"; return NULL;
	}
	return (tBlockRecipe*)j->pRecipe;
}

// The stubs for the islands of a method's blocks: [the instruction's own words][JIT_NATIVE_RESUME][block][entry]. They go after the last op of
// the method, which is never fallen into, and each exit of a block that is an island is patched to point at its stub.
typedef struct tIslandStubs_ {
	U32 n, cap;
	struct tIslandStub_ { const tOpWord *words; U32 len, exitWord, blockOfs, entry; } *item;
} tIslandStubs;
#define MAX_ISLANDS_PER_BLOCK 60
#define ISLAND_COST 6      // how many stencils of compiled code an island has to be worth (see FindBlockRegion)

typedef struct {
	U32 numInstr;
	const U32 *instrList, *start, *end, *val, *idx, *minSrc, *maxSrc;
	const U8 *kind, *isHard, *isTarget;
	const I32 *cilToInstr;          // CIL offset -> instruction number (or -1)
	tOps *pOps;
	tBlockRecipe * const *recipeOf; // per instruction: the callee to inline there (calls only)
	U32 inlineBase;                 // where inlined callees' frames go: just after this method's own parameters and locals
	U32 *pInlineFrame, *pInlineStack;   // the most frame and evaluation stack any inlined callee needs
	struct tIslandStubs_ *pStubs;       // where the islands' stubs are collected (they are put after the end of the method's ops)
	tMD_MethodDef *pMethodDef;          // the method these instructions are from
} tBlockCtx;

static int IsBranchKind(int kind) { return kind == K_BR || kind == K_BRT || kind == K_BRF || kind == K_BRT8 || kind == K_BRF8 || kind == K_BCC || kind == K_FBCC || kind == K_LBCC || kind == K_DBCC; }

// The instructions [k, k+n) that become one native block, or n = 0. The region is the longest run of instructions with
// stencils (branches included), cut back until it can be entered only at its start and left only through its exits:
//  * a branch target inside the region must be reached only from inside it (every branch to it is in the region);
//  * a try/handler/filter boundary, or an instruction with a debugger sequence point, ends it.
// The targets of backward branches inside it are the places it can be entered again after giving up the processor.
static U32 lastRegionRaw;        // and before the island rule too: a whole method that is a loop-free block with islands is a recipe whatever they cost
static U32 lastRegionLength;     // how long the last region was, before the minimum size was applied (a whole method is made a recipe at any length)
static U32 FindBlockRegion(const tBlockCtx *c, U32 k) {
	U32 e = k, t, s, hasLoop = 0, weight = 0, islands = 0;
	int again;
	static int noBranches = -1;
	if (noBranches < 0) { noBranches = (getenv("DNA_NO_BLOCK_BRANCHES") != NULL); }     // for comparing: blocks without loops
	while (e < c->numInstr && e - k < BLOCK_MAX && StencilIdFor(c->kind[e], c->idx[e]) >= 0 &&
			!(noBranches && IsBranchKind(c->kind[e])) && (e == k || !c->isHard[c->instrList[e]]) &&
			(c->kind[e] != K_ISLAND || islands < MAX_ISLANDS_PER_BLOCK)) {
		if (c->kind[e] == K_ISLAND) { islands++; }
		e++;
	}
	do {
		again = 0;
		for (t = k + 1; t < e && !again; t++) {
			if (c->minSrc[t] <= c->maxSrc[t] && (c->minSrc[t] < k || c->maxSrc[t] >= e)) {
				e = t;      // something outside the region jumps to t: the region ends before it
				again = 1;
			}
		}
	} while (again);
	// Islands cost: each one is an exit, a stub, a resume and an entry again, a good deal more than the interpreter calling something directly.
	// So a region with islands has to have that much more compiled around them; if it does not, it ends at its first island, which makes the
	// pieces on either side (if they are worth it) blocks of their own, as they were before islands existed.
	lastRegionRaw = e - k;
	for (;;) {
		U32 numIsl = 0;
		// (nothing outside the region may jump into the middle of it. This is checked again every time the region has been made shorter:
		// a jump that came from inside it may now come from outside, and would land on the start of the block instead of the target.)
		do {
			again = 0;
			for (t = k + 1; t < e && !again; t++) {
				if (c->minSrc[t] <= c->maxSrc[t] && (c->minSrc[t] < k || c->maxSrc[t] >= e)) {
					e = t;
					again = 1;
				}
			}
		} while (again);
		hasLoop = 0; weight = 0;
		// a block does not begin with an island (it would only leave at once) and does not end with one (it would only come back to the end)
		while (e > k && c->kind[e - 1] == K_ISLAND) { e--; }
		if (e > k && c->kind[k] == K_ISLAND) { lastRegionLength = 0; return 0; }
		for (s = k; s < e; s++) {
			if (IsBranchKind(c->kind[s])) {
				I32 tt = c->cilToInstr[c->val[s]];
				if (tt >= (I32)k && (U32)tt < e && (U32)tt <= s) { hasLoop = 1; }
			}
			// an inlinable call saves a whole call (a frame and its setup): worth a block by itself
			weight += (c->kind[s] == K_CALLD || c->kind[s] == K_CALLVIRTD) ? 16 : (c->kind[s] == K_ISLAND ? 0 : 1);
			if (c->kind[s] == K_ISLAND) { numIsl++; }
		}
		if (numIsl == 0 || weight >= (hasLoop ? BLOCK_MIN_LOOP : BlockMinStraight()) + ISLAND_COST * numIsl) { break; }
		for (s = k; s < e && c->kind[s] != K_ISLAND; s++) { }
		e = s;
	}
	lastRegionLength = e - k;
	if (getenv("DNA_FUSION_DEBUG") != NULL && e - k >= 4) {
		fprintf(stderr, "  region at instruction %u: %u instructions, %s, %s, ends before instruction %u (kind %d)\n", k, e - k,
			hasLoop ? "has a loop" : "straight", (weight >= (hasLoop ? BLOCK_MIN_LOOP : BlockMinStraight())) ? "BLOCK" : "too short",
			e, e < c->numInstr ? (int)c->kind[e] : -1);
	}
	return (weight >= (hasLoop ? BLOCK_MIN_LOOP : BlockMinStraight())) ? e - k : 0;
}

// Compile the region [k, k+n) and write the block instruction: [JIT_NATIVE_BLOCK][code][numExits][exit target]*
// The exit targets are CIL offsets until the branch fixup: each is the operand word of the branch it came from, so its
// new position is recorded in remap (and the operand words of branches that stayed inside are marked as gone).
// A call with a recipe becomes: (for callvirt, a check of `this`), the arguments popped into the callee's frame, which is in the
// caller's frame after its own locals, that frame's locals cleared, then the callee's stencils with their frame offsets moved there.
// If this region is a whole method that qualifies, *ppRecipe is a recipe for it (else NULL).
static int EmitBlock(const tBlockCtx *c, U32 k, U32 n, tOpWord *newP, U32 *pNewOfs, U32 *remap, tBlockRecipe **ppRecipe, int recipeOnly) {
	U32 cap = 2 * n + 1, j, i, e, p;
	unsigned *ids;
	U32 *holes, *holes1, *holes2, *srcInstr, *stencilOf, *exitSrc, *entries;
	U64 *pool64 = NULL;               // the 64-bit constants of the register pass
	U32 numPool = 0;
	int *tgt;
	U32 nb = 0, numExits = 0, numEntries = 1, numIslands = 0, *ientry, *ilen, *exitSten, blockOfs = 0, ri;
	char *endJump;               // per stencil: a branch to the return that follows the region (see the recipe)
	U32 numEndJumps = 0;
	const tOpWord **iwords;      // per stencil: if it is an island, the words of the instruction the interpreter is to run there
	void *code;
	int ok = 0, anyBackward = 0, entryBad = 0;      // a block with a loop in it needs the time-slice accounting

	*ppRecipe = NULL;
	for (j = 0; j < n; j++) {
		if (c->kind[k + j] == K_FFI) { cap += 3; }
		if (c->kind[k + j] == K_CALLD || c->kind[k + j] == K_CALLVIRTD) {
			tMD_MethodDef *m = (tMD_MethodDef*)c->pOps->p[c->start[k + j] + 1];
			cap += 3 + m->parameterStackSize / 4 + m->pJITted->localsStackSize / 4 + 2 + c->recipeOf[k + j]->n;
		}
	}
	ids = (unsigned*)malloc(cap * sizeof(unsigned));
	holes = (U32*)malloc(cap * sizeof(U32));
	holes1 = (U32*)calloc(cap, sizeof(U32));        // (the second and third operands of the stencils that have them: see the register pass)
	holes2 = (U32*)calloc(cap, sizeof(U32));
	srcInstr = (U32*)malloc(cap * sizeof(U32));
	stencilOf = (U32*)malloc((n + 1) * sizeof(U32));
	exitSrc = (U32*)malloc(cap * sizeof(U32));
	entries = (U32*)malloc((cap + 1) * sizeof(U32));
	tgt = (int*)malloc(cap * sizeof(int));
	iwords = (const tOpWord**)calloc(cap, sizeof(*iwords));
	endJump = (char*)calloc(cap, 1);
	ilen = (U32*)calloc(cap, sizeof(U32));
	ientry = (U32*)calloc(cap, sizeof(U32));
	exitSten = (U32*)malloc(cap * sizeof(U32));
#define ADD(id, hole, src, target) do { ids[nb] = (unsigned)(id); holes[nb] = (hole); srcInstr[nb] = (src); tgt[nb] = (target); nb++; } while (0)

	for (j = 0; j < n; j++) {
		stencilOf[j] = nb;
		if (c->kind[k + j] == K_LDA && j + 1 < n && c->kind[k + j + 1] == K_LDFLD8 && !c->isTarget[c->instrList[k + j + 1]]) {
			// the same for an 8-byte field: a load of the 8 bytes at L+F
			ADD(ST_LDP, c->val[k + j] + c->val[k + j + 1], 0xffffffff, -1);
			stencilOf[++j] = nb - 1;
			continue;
		}
		if (c->kind[k + j] == K_LDA && j + 1 < n && c->kind[k + j + 1] == K_LDFLD4 && !c->isTarget[c->instrList[k + j + 1]]) {
			// ldloca L; ldfld F: the address is a frame address, never null, so this is a load of the slot L+F
			ADD(ST_LDL, c->val[k + j] + c->val[k + j + 1], 0xffffffff, -1);
			stencilOf[++j] = nb - 1;
			continue;
		}
		if (c->kind[k + j] == K_LDC8) {
			// ldc.i8 / ldc.r8: the operand word holds all 64 bits (this target has 8-byte op words); store them as two halves
			U64 w = (U64)c->pOps->p[c->start[k + j] + 1];
			ADD(ST_LDC8LO, (U32)w, 0xffffffff, -1);
			ADD(ST_LDC8HI, (U32)(w >> 32), 0xffffffff, -1);
			continue;
		}
		if (c->kind[k + j] == K_CALLD || c->kind[k + j] == K_CALLVIRTD) {
			tMD_MethodDef *m = (tMD_MethodDef*)c->pOps->p[c->start[k + j] + 1];
			tBlockRecipe *r = c->recipeOf[k + j];
			tJITted *cj = m->pJITted;
			U32 base = c->inlineBase, zo, ze, baseIdx, frame = m->parameterStackSize + cj->localsStackSize;
			if (c->kind[k + j] == K_CALLVIRTD) {
				ADD(ST_CHKTHIS, (U32)(0 - m->parameterStackSize), 0xffffffff, -1);       // callvirt checks `this`
			}
			// the arguments are on the stack in order, so the last is on top: pop them into the callee's parameters, last first
			for (p = m->numberOfParameters; p-- > 0; ) {
				U32 size = m->pParams[p].size, off = base + m->pParams[p].offset, u;
				if (size == 8) {
					ADD(ST_STP, off, 0xffffffff, -1);
				} else if (size == 4) {
					ADD(ST_STL, off, 0xffffffff, -1);
				} else {
					for (u = size / 4; u-- > 0; ) { ADD(ST_STL, off + 4 * u, 0xffffffff, -1); }
				}
			}
			// a real call gives the callee's locals zeroed memory
			zo = base + m->parameterStackSize;
			ze = base + frame;
			while (zo + 8 <= ze) { ADD(ST_ZERO8, zo, 0xffffffff, -1); zo += 8; }
			while (zo + 4 <= ze) { ADD(ST_ZERO4, zo, 0xffffffff, -1); zo += 4; }
			// the callee's body, with its frame moved to where its frame is now
			baseIdx = nb;
			ri = 0;
			for (i = 0; i < r->n; i++) {
				ADD(r->ids[i], IsFrameStencil(r->ids[i]) ? r->holes[i] + base : r->holes[i], 0xffffffff, r->tgt[i] >= 0 ? r->tgt[i] + (int)baseIdx : -1);
				if (ri < r->numIslands && r->islandAt[ri] == i) {
					// an island of the callee: the interpreter runs the same instruction here, in the caller's frame, and the block goes on after it
					iwords[nb - 1] = r->islandWords[ri];
					ilen[nb - 1] = r->islandLen[ri];
					ientry[nb - 1] = numEntries;
					entries[numEntries++] = nb;
					numIslands++;
					ri++;
				}
			}
			if (!recipeOnly) { NativeBlock_CountInlined(); }
			if (r->hasEnd) { ADD(ST_ZERO4, base, 0xffffffff, -1); }      // somewhere for the recipe's jumps to its end to land (the callee's frame is dead now)
			if (frame > *c->pInlineFrame) { *c->pInlineFrame = frame; }
			if (cj->maxStack > *c->pInlineStack) { *c->pInlineStack = cj->maxStack; }
			continue;
		}
		if (c->kind[k + j] == K_FFI) {
			// a call of a C function: its address into r11 (two 32-bit halves), then the stencil of its signature, which takes the arguments
			// off the evaluation stack, calls it and puts the result there
			const tFFIEntry *pFfi = (const tFFIEntry*)c->pOps->p[c->start[k + j] + 1];
			U64 fnAddr = (U64)(uintptr_t)pFfi->fn;
			ADD(ffiStFnLo, (U32)fnAddr, 0xffffffff, -1);
			ADD(ffiStFnHi, (U32)(fnAddr >> 32), 0xffffffff, -1);
			ADD(NativeBlock_FindStencil(pFfi->stencil), 0, 0xffffffff, -1);
			continue;
		}
		if (c->kind[k + j] == K_ISLAND) {
			// leave the block here, let the interpreter run this one instruction, and come back at the next stencil
			ADD(ST_J, 0, 0xffffffff, -1);
			iwords[nb - 1] = c->pOps->p + c->start[k + j];
			ilen[nb - 1] = c->end[k + j] - c->start[k + j];
			ientry[nb - 1] = numEntries;
			entries[numEntries++] = nb;
			numIslands++;
			continue;
		}
		ADD(StencilIdFor(c->kind[k + j], c->idx[k + j]), IsBranchKind(c->kind[k + j]) ? 0 : c->val[k + j],
			IsBranchKind(c->kind[k + j]) ? k + j : 0xffffffff, -1);
	}
	if (!recipeOnly) {
		// The register pass: keep values in registers instead of on the evaluation stack in memory (VStack.c). Where something can jump or
		// be entered, nothing may be held in a register, and after it the places that point into the list are moved.
		unsigned char *flushAt = (unsigned char*)calloc(nb + 1, 1);
		U32 *remap = (U32*)malloc((nb + 1) * sizeof(U32));
		tStencilList list;
		U32 newNb;
		list.ids = ids; list.hole0 = holes; list.hole1 = holes1; list.hole2 = holes2; list.srcInstr = srcInstr;
		list.iwords = iwords; list.ilen = ilen; list.ientry = ientry; list.endJump = endJump; list.tgt = tgt;
		for (j = 0; j < n; j++) {
			if (c->isTarget[c->instrList[k + j]]) { flushAt[stencilOf[j]] = 1; }
		}
		for (e = 1; e < numEntries; e++) { flushAt[entries[e]] = 1; }
		for (i = 0; i < nb; i++) {
			if (tgt[i] >= 0 && (U32)tgt[i] <= nb) { flushAt[tgt[i]] = 1; }       // (a branch inside an inlined recipe)
		}
		newNb = VStack_Run(&list, nb, flushAt, remap, &pool64, &numPool);
		for (j = 0; j < n; j++) { stencilOf[j] = remap[stencilOf[j]]; }
		for (e = 1; e < numEntries; e++) { entries[e] = remap[entries[e]]; }
		nb = newNb;
		free(flushAt); free(remap);
	}
	entries[0] = 0;
	for (i = 0; i < nb; i++) {
		if (iwords[i] != NULL) {
			// an island is always an exit
			exitSrc[numExits] = 0xffffffff;
			exitSten[numExits] = i;
			tgt[i] = (int)(nb + numExits);
			numExits++;
		} else if (srcInstr[i] != 0xffffffff) {
			I32 t = c->cilToInstr[c->val[srcInstr[i]]];
			if (t >= (I32)k && t < (I32)(k + n)) {
				tgt[i] = (int)stencilOf[t - k];
				if ((U32)tgt[i] <= i) { anyBackward = 1; }
				if ((U32)tgt[i] <= i && tgt[i] != 0) {
					// a backward branch: where it goes is a place the block can be entered again
					for (e = 0; e < numEntries && entries[e] != (U32)tgt[i]; e++) { }
					if (e == numEntries) { entries[numEntries++] = (U32)tgt[i]; }
				}
			} else {
				if (t == (I32)(k + n) && k + n < c->numInstr && c->kind[k + n] == K_RET) { endJump[i] = 1; numEndJumps++; }
				exitSrc[numExits] = srcInstr[i];
				tgt[i] = (int)(nb + numExits);
				numExits++;
			}
		}
	}
#undef ADD
	if (getenv("DNA_FUSION_DEBUG") != NULL && !recipeOnly) { NativeBlock_Dump(ids, holes, tgt, nb); }
	for (e = 1; e < numEntries; e++) {
		if (entries[e] >= nb) { entryBad = 1; }       // (a block that would end with an island has no stencil to come back to)
	}
	code = (recipeOnly || entryBad) ? NULL : NativeBlock_Compile(ids, holes, holes1, holes2, tgt, nb, numExits, entries, numEntries, pool64, numPool);
	if (code != NULL || recipeOnly) {
		if (code != NULL) {
		blockOfs = *pNewOfs;
		NativeBlock_CountIslands(numIslands);
		newP[(*pNewOfs)++] = Translate((anyBackward || numIslands) ? JIT_NATIVE_LOOP : JIT_NATIVE_BLOCK, 0);   // (only the loop form can be entered again)
		newP[(*pNewOfs)++] = (tOpWord)(uintptr_t)code;
		newP[(*pNewOfs)++] = (tOpWord)numExits;
		for (j = 0; j < n; j++) {
			if (IsBranchKind(c->kind[k + j])) {
				remap[c->start[k + j] + 1] = 0xfffffffe;       // (replaced below if this branch is an exit)
			}
		}
		for (i = 0; i < numExits; i++) {
			U32 ex = exitSrc[i];
			if (ex == 0xffffffff) {
				// the exit goes to the island's stub, whose place is not known until the method's ops are all laid out
				tIslandStubs *st = c->pStubs;
				if (st->n == st->cap) {
					st->cap = st->cap ? st->cap * 2 : 16;
					st->item = (struct tIslandStub_*)realloc(st->item, st->cap * sizeof(st->item[0]));
				}
				st->item[st->n].words = iwords[exitSten[i]];
				st->item[st->n].len = ilen[exitSten[i]];
				st->item[st->n].exitWord = *pNewOfs;
				st->item[st->n].blockOfs = blockOfs;
				st->item[st->n].entry = ientry[exitSten[i]];
				st->n++;
				newP[(*pNewOfs)++] = 0;
			} else {
				U32 pos = c->start[ex] + 1;
				remap[pos] = *pNewOfs;
				newP[(*pNewOfs)++] = c->pOps->p[pos];
			}
		}
		}
		if (!anyBackward && numExits == numIslands + numEndJumps && (code != NULL || recipeOnly) && (numEndJumps == 0 || c->inlineBase >= 4)) {
			// loop-free and leaving only by islands: it can be a recipe, if it turns out to be the whole method (the caller decides)
			tBlockRecipe *r = (tBlockRecipe*)malloc(sizeof(tBlockRecipe));
			U32 q, ni = 0;
			r->n = nb;
			r->numIslands = numIslands;
			r->islandAt = (U32*)malloc((numIslands + 1) * sizeof(U32));
			r->islandWords = (tOpWord**)malloc((numIslands + 1) * sizeof(tOpWord*));
			r->islandLen = (U32*)malloc((numIslands + 1) * sizeof(U32));
			r->hasEnd = numEndJumps > 0;
			for (q = 0; q < nb; q++) {
				if (iwords[q] != NULL) {
					// the words of the instruction. An ldstr looks its string up in the metadata of the method it is running in, and this
					// will run in another: so it gets the metadata of this method with it.
					U32 len = ilen[q];
					tOpWord *w = (tOpWord*)malloc((len + 1) * sizeof(tOpWord));
					if (iwords[q][0] == fc.loadString) {
						w[0] = fc.loadStringMd; w[1] = (tOpWord)(uintptr_t)c->pMethodDef->pMetaData; w[2] = iwords[q][1]; len = 3;
					} else {
						memcpy(w, iwords[q], len * sizeof(tOpWord));
					}
					r->islandAt[ni] = q; r->islandWords[ni] = w; r->islandLen[ni] = len;
					ni++;
				}
			}
			r->ids = (unsigned*)malloc(nb * sizeof(unsigned)); memcpy(r->ids, ids, nb * sizeof(unsigned));
			r->holes = (U32*)malloc(nb * sizeof(U32)); memcpy(r->holes, holes, nb * sizeof(U32));
			r->tgt = (int*)malloc(nb * sizeof(int)); memcpy(r->tgt, tgt, nb * sizeof(int));
			for (q = 0; q < nb; q++) {
				if (iwords[q] != NULL) { r->tgt[q] = -1; }          // (an island is re-made where the recipe is used, not a branch)
				if (endJump[q]) { r->tgt[q] = (int)nb; }            // (the end of the recipe)
			}
			*ppRecipe = r;
		}
		ok = (code != NULL);
	}
	free(pool64); free(ids); free(holes); free(holes1); free(holes2); free(srcInstr); free(stencilOf); free(exitSrc); free(entries); free(tgt); free(iwords); free(ilen); free(ientry); free(exitSten); free(endJump);
	return ok;
}
#endif

// Set while the interpreter's version of a method that was compiled to wasm is made (see JIT_BuildInterpreterVersion): the CIL offsets that the interpreter
// takes over at (they must start an op, so fusion treats them as branch targets), and those of the loop headers where it goes back into compiled code.
static const U32 *jitKeep = NULL; static U32 jitNumKeep = 0;
static const U32 *jitOsr = NULL; static U32 jitNumOsr = 0;

static void FuseOps(tOps *pOps, tOps *pBranches, U32 *pJITOffsets, const U32 *instrList, U32 numInstr,
		tJITted *pJITted, U32 codeSize, tMD_MethodDef *pMethodDef) {
	static int enabled = -1;
	U32 k, i, newOfs, newCap, *start, *end, *val, *idx, *newJIT, *remap;
	int fusedAny = 0;
#if NATIVE_BLOCKS
	tBlockRecipe **recipeOf = NULL;
	U32 inlineFrame = 0, inlineStack = 0;
	tIslandStubs stubs = { 0, 0, NULL };
	int allowIslands;
	tBlockCtx blockCtx;
#endif
	U8 *kind, *isTarget, *isHard;
	I32 *cilToInstr;
	U32 *instrOfWord, *minSrc, *maxSrc;
	tOpWord *newP;
	I32 *newSeq;

	if (enabled < 0) {
		enabled = (getenv("DNA_NO_FUSION") == NULL);
	}
	if (!enabled || numInstr < 2) {
		return;
	}
	if (!fc.ready) {
		InitFuseCodes();
	}

	// everything that may be jumped to, or that bounds a region
	isTarget = (U8*)calloc(codeSize + 2, 1);
	isHard = (U8*)calloc(codeSize + 2, 1);       // targets that no block may contain: region boundaries, sequence points
	for (i = 0; i < pBranches->ofs; i++) {
		U32 t = (U32)pOps->p[pBranches->p[i]];
		if (t <= codeSize) { isTarget[t] = 1; }
	}
	for (i = 0; i < jitNumKeep; i++) {
		if (jitKeep[i] <= codeSize) { isTarget[jitKeep[i]] = 1; }
	}
	for (i = 0; i < pJITted->numExceptionHandlers; i++) {
		tExceptionHeader *pEx = &pJITted->pExceptionHeaders[i];
		U32 marks[5];
		U32 j, n = 0;
		marks[n++] = pEx->tryStart;
		marks[n++] = pEx->tryStart + pEx->tryEnd;            // (still lengths, not yet converted)
		marks[n++] = pEx->handlerStart;
		marks[n++] = pEx->handlerStart + pEx->handlerEnd;
		if (pEx->flags == COR_ILEXCEPTION_CLAUSE_FILTER) { marks[n++] = pEx->u.filterOffset; }
		for (j = 0; j < n; j++) {
			if (marks[j] <= codeSize) { isTarget[marks[j]] = 1; isHard[marks[j]] = 1; }
		}
	}

	start = (U32*)malloc(numInstr * sizeof(U32));
	end = (U32*)malloc(numInstr * sizeof(U32));
	val = (U32*)calloc(numInstr, sizeof(U32));
	idx = (U32*)calloc(numInstr, sizeof(U32));
	kind = (U8*)calloc(numInstr, 1);
	newJIT = (U32*)malloc(numInstr * sizeof(U32));
	for (k = 0; k < numInstr; k++) {
		start[k] = pJITOffsets[instrList[k]];
		end[k] = (k + 1 < numInstr) ? pJITOffsets[instrList[k + 1]] : pOps->ofs;
		if (pOps->pSequencePoints != NULL && end[k] > start[k] && pOps->pSequencePoints[start[k]] >= 0) {
			isTarget[instrList[k]] = 1;       // keep a debugger's statement boundaries where they are
			isHard[instrList[k]] = 1;
		}
		kind[k] = (U8)ClassifyOps(pOps->p + start[k], end[k] - start[k], &val[k], &idx[k]);
	}

	// Who branches to whom, for the native blocks: for each instruction, the first and last instruction that branches to it
	cilToInstr = (I32*)malloc((codeSize + 2) * sizeof(I32));
	for (i = 0; i <= codeSize + 1; i++) { cilToInstr[i] = -1; }
	for (k = 0; k < numInstr; k++) { cilToInstr[instrList[k]] = (I32)k; }
	instrOfWord = (U32*)calloc(pOps->ofs + 1, sizeof(U32));
	for (k = 0; k < numInstr; k++) {
		for (i = start[k]; i < end[k]; i++) { instrOfWord[i] = k; }
	}
	minSrc = (U32*)malloc(numInstr * sizeof(U32));
	maxSrc = (U32*)calloc(numInstr, sizeof(U32));
	for (k = 0; k < numInstr; k++) { minSrc[k] = 0xffffffff; }
	for (i = 0; i < pBranches->ofs; i++) {
		U32 pos = pBranches->p[i], target = (U32)pOps->p[pos];
		if (target <= codeSize && cilToInstr[target] >= 0) {
			U32 s = instrOfWord[pos], t = (U32)cilToInstr[target];
			if (s < minSrc[t]) { minSrc[t] = s; }
			if (s > maxSrc[t]) { maxSrc[t] = s; }
		}
	}
#if NATIVE_BLOCKS
	blockCtx.numInstr = numInstr; blockCtx.instrList = instrList; blockCtx.start = start; blockCtx.val = val;
	blockCtx.idx = idx; blockCtx.minSrc = minSrc; blockCtx.maxSrc = maxSrc; blockCtx.kind = kind;
	blockCtx.isHard = isHard; blockCtx.isTarget = isTarget; blockCtx.cilToInstr = cilToInstr; blockCtx.pOps = pOps;
	// Which calls a block may contain the callee of (compiling the callee if need be): a call that cannot be inlined is not something
	// a block can contain, so its kind is cleared before any region is looked for
	// Islands are instructions that the interpreter runs in the middle of a block. A stub that lets it do so is after the end of the method's ops,
	// outside every try range, so a method with exception handlers has none (a throw from an island would not find its handler).
	{
		static int noIslands = -1;
		if (noIslands < 0) { noIslands = (getenv("DNA_NO_ISLANDS") != NULL); }
		allowIslands = !noIslands && pJITted->numExceptionHandlers == 0;
	}
	recipeOf = (tBlockRecipe**)calloc(numInstr, sizeof(tBlockRecipe*));
	for (k = 0; k < numInstr; k++) {
		if (kind[k] == K_CALLD || kind[k] == K_CALLVIRTD) {
			recipeOf[k] = InlineRecipeFor((tMD_MethodDef*)pOps->p[start[k] + 1], kind[k] == K_CALLVIRTD, pMethodDef, allowIslands);
			if (recipeOf[k] == NULL) { kind[k] = K_ISLAND; }        // (a call that is not inlined is an island, if there are any)
		}
		if (kind[k] == K_ISLAND && !allowIslands) { kind[k] = K_OTHER; }
	}
	blockCtx.end = end;
	blockCtx.pMethodDef = pMethodDef;
	blockCtx.pStubs = &stubs;
	blockCtx.recipeOf = recipeOf;
	blockCtx.inlineBase = pMethodDef->parameterStackSize + pJITted->localsStackSize;
	blockCtx.pInlineFrame = &inlineFrame;
	blockCtx.pInlineStack = &inlineStack;
#endif

	// Every fused form is no longer than what it replaces, except LOAD2 (two one-word slot loads become three words),
	// so the output can be longer than the input by half at most.
	newCap = pOps->ofs + pOps->ofs / 2 + 8;
	if (getenv("DNA_FUSION_DEBUG") != NULL) {
		fprintf(stderr, "fuse %s:", Sys_GetMethodDesc(pMethodDef));
		for (k = 0; k < numInstr; k++) {
			fprintf(stderr, " %d%s", kind[k], isTarget[instrList[k]] ? "T" : "");
		}
		fprintf(stderr, "\n");
	}
	newP = (tOpWord*)malloc(newCap * sizeof(tOpWord));
	newSeq = (pOps->pSequencePoints != NULL) ? (I32*)malloc(newCap * sizeof(I32)) : NULL;
	remap = (U32*)malloc((pOps->ofs + 1) * sizeof(U32));
	for (i = 0; i <= pOps->ofs; i++) {
		remap[i] = 0xffffffff;
	}
	newOfs = 0;

#define KIND(j) ((k + (j) < numInstr) ? kind[k + (j)] : K_OTHER)
#define FREE(j) (!isTarget[instrList[k + (j)]])
	for (k = 0; k < numInstr; ) {
		U32 consumed = 1, w, fusedStart = newOfs;
		I32 seq0 = (newSeq != NULL && end[k] > start[k]) ? pOps->pSequencePoints[start[k]] : -1;

		U32 blockN = 0;
#if NATIVE_BLOCKS
		if (NativeBlock_Enabled()) {
			blockN = FindBlockRegion(&blockCtx, k);
			if (k == 0 && lastRegionRaw + 1 == numInstr && kind[numInstr - 1] == K_RET && pJITted->numExceptionHandlers == 0) {
				// the whole method but its return is a loop-free block (which may have islands, and may be too short or too poor to be worth
				// a block of its own): a recipe, so that callers can contain it instead of calling it
				tBlockRecipe *recipe = NULL;
				EmitBlock(&blockCtx, 0, lastRegionRaw, newP, &newOfs, remap, &recipe, 1);
				pJITted->pRecipe = recipe;
			}
			if (blockN != 0) {
				tBlockRecipe *recipe = NULL;
				if (EmitBlock(&blockCtx, k, blockN, newP, &newOfs, remap, &recipe, 0)) {
					consumed = blockN;
					if (recipe != NULL) {
						free(recipe->ids); free(recipe->holes); free(recipe->tgt); free(recipe->islandAt); free(recipe->islandLen);
						free(recipe);        // (leaks the island words: the recipe of a whole method is the one made above)
					}
				} else {
					blockN = 0;
				}
			}
		}
#endif
		if (blockN != 0) {
			// done above
		} else if (KIND(0) == K_LOADL && KIND(1) == K_CONSTI && KIND(2) == K_BIN && FREE(1) && FREE(2) &&
				(idx[k + 2] == fc.addIdx || idx[k + 2] == fc.subIdx) && KIND(3) == K_STOREL && FREE(3) &&
				val[k + 3] == val[k]) {
			// ldloc a; ldc c; add/sub; stloc a   ->   a += c
			newP[newOfs++] = fc.inc;
			newP[newOfs++] = val[k];
			newP[newOfs++] = (idx[k + 2] == fc.addIdx) ? val[k + 1] : (U32)(0 - val[k + 1]);
			consumed = 4;
		} else if (KIND(0) == K_LOADL && KIND(1) == K_LOADL && KIND(2) == K_BIN && FREE(1) && FREE(2) &&
				fusedBins[idx[k + 2]].ll != 0) {
			newP[newOfs++] = Translate(fusedBins[idx[k + 2]].ll, 0);
			newP[newOfs++] = val[k];
			newP[newOfs++] = val[k + 1];
			consumed = 3;
		} else if (KIND(0) == K_LOADL && KIND(2) == K_BIN && FREE(1) && FREE(2) &&
				KIND(1) == (fusedBins[idx[k + 2]].isFloat ? K_CONSTF : K_CONSTI)) {
			newP[newOfs++] = Translate(fusedBins[idx[k + 2]].lc, 0);
			newP[newOfs++] = val[k];
			newP[newOfs++] = val[k + 1];
			consumed = 3;
		} else if (KIND(0) == K_LOADL && KIND(1) == K_LOADL && KIND(2) == K_BCC && FREE(1) && FREE(2)) {
			newP[newOfs++] = Translate(fusedBccs[idx[k + 2]].ll, 0);
			newP[newOfs++] = val[k];
			newP[newOfs++] = val[k + 1];
			remap[start[k + 2] + 1] = newOfs;               // the branch operand moves here
			newP[newOfs++] = pOps->p[start[k + 2] + 1];     // still a CIL offset: converted by the fixup that follows
			consumed = 3;
		} else if (KIND(0) == K_LOADL && KIND(1) == K_CONSTI && KIND(2) == K_BCC && FREE(1) && FREE(2)) {
			newP[newOfs++] = Translate(fusedBccs[idx[k + 2]].lc, 0);
			newP[newOfs++] = val[k];
			newP[newOfs++] = val[k + 1];
			remap[start[k + 2] + 1] = newOfs;
			newP[newOfs++] = pOps->p[start[k + 2] + 1];
			consumed = 3;
		} else if (KIND(0) == K_LOADL && KIND(1) == K_LOADL && FREE(1)) {
			newP[newOfs++] = fc.load2;
			newP[newOfs++] = val[k];
			newP[newOfs++] = val[k + 1];
			consumed = 2;
		}

		// (a block replaces what it covers even when that is one instruction: an inlined call is, and "consumed == 1" would copy it again)
		if (consumed > 1 || blockN != 0) {
			fusedAny = 1;
			// the fused instruction stands for instructions k .. k+consumed-1
			for (w = 0; w < consumed; w++) {
				newJIT[k + w] = fusedStart;
			}
			if (newSeq != NULL) {
				for (w = fusedStart; w < newOfs; w++) { newSeq[w] = -1; }
				newSeq[fusedStart] = seq0;
			}
		} else {
			// copied as it is
			newJIT[k] = newOfs;
			for (w = start[k]; w < end[k]; w++) {
				remap[w] = newOfs;
				if (newSeq != NULL) { newSeq[newOfs] = pOps->pSequencePoints[w]; }
				newP[newOfs++] = pOps->p[w];
			}
		}
		k += consumed;
	}
#undef KIND
#undef FREE

#if NATIVE_BLOCKS
	if (stubs.n > 0) {
		// the islands' stubs: [the instruction][JIT_NATIVE_RESUME block entry], after everything else (nothing falls into them)
		U32 q, extra = 0;
		for (q = 0; q < stubs.n; q++) { extra += stubs.item[q].len + 3; }
		if (newOfs + extra > newCap) {
			newCap = newOfs + extra + 8;
			newP = (tOpWord*)realloc(newP, newCap * sizeof(tOpWord));
			if (newSeq != NULL) { newSeq = (I32*)realloc(newSeq, newCap * sizeof(I32)); }
		}
		for (q = 0; q < stubs.n; q++) {
			U32 at = newOfs, w2;
			for (w2 = 0; w2 < stubs.item[q].len; w2++) {
				if (newSeq != NULL) { newSeq[newOfs] = -1; }
				newP[newOfs++] = stubs.item[q].words[w2];
			}
			if (newSeq != NULL) { newSeq[newOfs] = -1; newSeq[newOfs + 1] = -1; newSeq[newOfs + 2] = -1; }
			newP[newOfs++] = Translate(JIT_NATIVE_RESUME, 0);
			newP[newOfs++] = (tOpWord)stubs.item[q].blockOfs;
			newP[newOfs++] = (tOpWord)stubs.item[q].entry;
			newP[stubs.item[q].exitWord] = (tOpWord)at;           // the block's exit goes here
		}
		free(stubs.item);
		fusedAny = 1;
	}
#endif

	if (fusedAny) {
		// something was fused: install the compacted stream and remap everything that refers into it
		for (k = 0; k < numInstr; k++) {
			pJITOffsets[instrList[k]] = newJIT[k];
		}
		{
			U32 kept = 0;
			for (i = 0; i < pBranches->ofs; i++) {
				U32 r = remap[pBranches->p[i]];
				if (r == 0xfffffffe) { continue; }       // a branch that is now inside a native block: no operand word left
				Assert(r != 0xffffffff);
				pBranches->p[kept++] = r;
			}
			pBranches->ofs = kept;
		}
		free(pOps->p);
		pOps->p = newP;
		if (pOps->pSequencePoints != NULL) {
			free(pOps->pSequencePoints);
			pOps->pSequencePoints = newSeq;
		}
		pOps->ofs = newOfs;
	} else {
		free(newP);
		free(newSeq);
	}
#if NATIVE_BLOCKS
	if (inlineFrame > 0) {
		// the callees inlined into blocks run in a frame of their own after this method's locals, and on evaluation stack above this
		// method's: make room (maxStack is set at the end of JITit, and picks inlineExtraStack up there)
		pJITted->localsStackSize += inlineFrame;
		pJITted->inlineExtraStack += inlineStack;
	}
	free(recipeOf);
#endif
	free(isHard); free(cilToInstr); free(instrOfWord); free(minSrc); free(maxSrc);
	free(isTarget); free(start); free(end); free(val); free(idx); free(kind); free(newJIT); free(remap);
}

static tOpWord* JITit(tMD_MethodDef *pMethodDef, U8 *pCIL, U32 codeSize, tParameter *pLocals, tJITted *pJITted, U32 genCombinedOpcodes, I32 **ppSequencePoints, U32 **ppCilToOp) {
	U32 maxStack = pJITted->maxStack;
	U32 i;
	U32 cilOfs;
	tOps ops; // The JITted op-codes
	tOps branchOffsets; // Filled with all the branch instructions that need offsets fixing
	U32 *pJITOffsets;	// To store the JITted code offset of each CIL byte.
	U32 *instrList, numInstr;
						// Only CIL bytes that are the first byte of an instruction will have meaningful data
	tTypeStack **ppTypeStacks; // To store the evaluation stack state for forward jumps
	tOpWord *pFinalOps;
	tMD_TypeDef *pStackType;
	tTypeStack typeStack;
	U32 jmpPending = 0; // set while translating a jmp as "load args; call; ret"

#ifdef GEN_COMBINED_OPCODES
	tOps isDynamic;
#endif

	I32 i32Value;
	U32 u32Value, u32Value2, ofs;
	uConvFloat convFloat;
	uConvDouble convDouble;
	tMD_TypeDef *pTypeA, *pTypeB;
	PTR pMem;
	tMetaData *pMetaData;
    tDebugMetaData* pDebugMetadata;
    tDebugMetaDataEntry* pDebugMetadataEntry = NULL;
    int sequencePointIndex;

	pMetaData = pMethodDef->pMetaData;
    pDebugMetadata = pMetaData->debugMetadata;

    // TODO: Use a hash table as this is super slow
    if (pDebugMetadata != NULL) {
        tDebugMetaDataEntry* pEntry = pDebugMetadata->entries;

        while (pEntry != NULL) {
            // TODO: Compare namespace, type and module name
            if (strcmp(pEntry->pMethodName, pMethodDef->name) == 0) {
                if (pMethodDef->pParentType != NULL) {
                    if (strcmp(pEntry->pClassName, pMethodDef->pParentType->name) == 0 && strcmp(pEntry->pNamespaceName, pMethodDef->pParentType->nameSpace) == 0) {
                        pDebugMetadataEntry = pEntry;
                        break;
                    }
                }
                else {
                    pDebugMetadataEntry = pEntry;
                    break;
                }
            }
            pEntry = pEntry->next;
        }
    }
    
	pJITOffsets = malloc(codeSize * sizeof(U32));
	// The CIL offset of each instruction translated, in order (for the fusion pass)
	instrList = (U32*)malloc((codeSize + 1) * sizeof(U32));
	numInstr = 0;
	// + 1 to handle cases where the stack is being restored at the last instruction in a method
	ppTypeStacks = malloc((codeSize + 1) * sizeof(tTypeStack*));
	memset(ppTypeStacks, 0, (codeSize + 1) * sizeof(tTypeStack*));
	typeStack.maxBytes = 0;
	typeStack.ofs = 0;
	typeStack.ppTypes = malloc(maxStack * sizeof(tMD_TypeDef*));
    sequencePointIndex = 0;

	// Set up all exception 'catch' blocks with the correct stack information,
	// So they'll have just the exception type on the stack when entered
	for (i=0; i<pJITted->numExceptionHandlers; i++) {
		tExceptionHeader *pEx;

		pEx = &pJITted->pExceptionHeaders[i]; 
		if (pEx->flags == COR_ILEXCEPTION_CLAUSE_EXCEPTION) {
			tTypeStack *pTypeStack;

			// The handler starts with the exception object on the stack, typed as the catch type, and its first
			// instruction (usually `pop` or `stloc`) is sized from that type. Until the type is filled in its
			// stackSize is 0, so the `pop` removed nothing: the exception reference stayed on the evaluation
			// stack, which the method's maximum stack did not allow for, and the next pushes overflowed it onto
			// the parameters. (This depended on whether the exception type had been used before this method was
			// compiled.)
			MetaData_Fill_TypeDef(pEx->u.pCatchTypeDef, NULL, NULL);
			ppTypeStacks[pEx->handlerStart] = pTypeStack = TMALLOC(tTypeStack);
			pTypeStack->maxBytes = sizeof(void*);   // the exception reference pushed on entry (was a hard-coded 4)
			pTypeStack->ofs = 1;
			pTypeStack->ppTypes = TMALLOC(tMD_TypeDef*);
			pTypeStack->ppTypes[0] = pEx->u.pCatchTypeDef;
		} else if (pEx->flags == COR_ILEXCEPTION_CLAUSE_FILTER) {
			// Both the filter code and the handler it guards are entered with the exception object
			// (typed as plain object: the filter is what decides whether it is wanted) on the stack.
			U32 entries[2];
			U32 k;
			entries[0] = pEx->u.filterOffset;
			entries[1] = pEx->handlerStart;
			for (k = 0; k < 2; k++) {
				tTypeStack *pTypeStack;

				ppTypeStacks[entries[k]] = pTypeStack = TMALLOC(tTypeStack);
				pTypeStack->maxBytes = sizeof(void*);   // the exception reference pushed on entry (was a hard-coded 4)
				pTypeStack->ofs = 1;
				pTypeStack->ppTypes = TMALLOC(tMD_TypeDef*);
				pTypeStack->ppTypes[0] = types[TYPE_SYSTEM_OBJECT];
			}
		}
	}

	// A catch or filter is entered with the exception reference on the evaluation stack, and that push is not
	// made by any instruction, so nothing else counts it towards the method's maximum stack (restoring a
	// saved type stack never raises the running maximum). Count it here. On a 32-bit target a reference is
	// 4 bytes, which some other instruction nearly always needs anyway, so it never showed; on 64-bit it
	// overflowed the evaluation stack into the parameters that sit right after it.
	if (pJITted->numExceptionHandlers > 0 && typeStack.maxBytes < sizeof(void*)) {
		typeStack.maxBytes = sizeof(void*);
	}

	InitOps(ops, 32);
	InitOps(branchOffsets, 16);
#ifdef GEN_COMBINED_OPCODES
	InitOps(isDynamic, 32);
#endif

	cilOfs = 0;

	int nextOpSequencePoint = -1;
	do {
		U8 op;

		// Set the JIT offset for this CIL opcode
		pJITOffsets[cilOfs] = ops.ofs;
		instrList[numInstr++] = cilOfs;
		{
			U32 oi;
			for (oi = 0; oi < jitNumOsr; oi++) {
				if (jitOsr[oi] == cilOfs) {
					int savedSeq = nextOpSequencePoint;
					nextOpSequencePoint = -1;
					PushOpParam(JIT_WASM_OSR, cilOfs);
					nextOpSequencePoint = savedSeq;
					break;
				}
			}
		}

        U32 pcilOfs = cilOfs;

		op = pCIL[cilOfs++];
		//printf("Opcode: 0x%02x\n", op);
        if (pDebugMetadataEntry != NULL && sequencePointIndex < pDebugMetadataEntry->sequencePointsCount) {
            U32 spOffset = pDebugMetadataEntry->sequencePoints[sequencePointIndex];
            if (spOffset == pcilOfs) {
                nextOpSequencePoint = sequencePointIndex;
                sequencePointIndex++;
            } else {
                nextOpSequencePoint = -1;
            }
        }
        
		switch (op) {
			case CIL_NOP:
                {
                    PushOp(JIT_NOP);
                }
				break;

			case CIL_BREAK:
				// A debugger breakpoint instruction; there is no attached debugger to stop in.
				PushOp(JIT_NOP);
				break;

			case CIL_PREFIX7:
			case CIL_PREFIX6:
			case CIL_PREFIX5:
			case CIL_PREFIX4:
			case CIL_PREFIX3:
			case CIL_PREFIX2:
			case CIL_PREFIXREF:
				// Reserved by ECMA-335 as encodings for future instruction prefixes; they are not
				// instructions and cannot appear in a valid method body.
				Crash("JITit(): reserved op-code 0x%02x is not valid in a method body", op);
				break;

			case CIL_MKREFANY:
				{
					tMD_TypeDef *pRefType;

					PopStackTypeDontCare(); // the address
					u32Value = GetUnalignedU32(pCIL, &cilOfs);
					pRefType = MetaData_GetTypeDefFromDefRefOrSpec(pMethodDef->pMetaData, u32Value, pMethodDef->pParentType->ppClassTypeArgs, pMethodDef->ppMethodTypeArgs);
					MetaData_Fill_TypeDef(pRefType, NULL, NULL);
					MetaData_Fill_TypeDef(types[TYPE_SYSTEM_TYPEDREFERENCE], NULL, NULL);
					PushOp(JIT_MKREFANY);
					PushPTR(pRefType);
					PushStackType(types[TYPE_SYSTEM_TYPEDREFERENCE]);
				}
				break;

			case CIL_REFANYVAL:
				{
					tMD_TypeDef *pRefType;

					PopStackTypeDontCare(); // the TypedReference
					u32Value = GetUnalignedU32(pCIL, &cilOfs);
					pRefType = MetaData_GetTypeDefFromDefRefOrSpec(pMethodDef->pMetaData, u32Value, pMethodDef->pParentType->ppClassTypeArgs, pMethodDef->ppMethodTypeArgs);
					MetaData_Fill_TypeDef(pRefType, NULL, NULL);
					PushOp(JIT_REFANYVAL);
					PushPTR(pRefType);
					PushStackType(types[TYPE_SYSTEM_INTPTR]);
				}
				break;

			case CIL_CALLI:
				{
					// calli <call-site signature>: ..., args, function pointer -> ..., result
					tMD_StandAloneSig *pCallSig;
					SIG callSig;
					U32 callConv, numCallParams;
					tMD_TypeDef *pCallRetType;

					u32Value = GetUnalignedU32(pCIL, &cilOfs);
					pCallSig = (tMD_StandAloneSig*)MetaData_GetTableRow(pMetaData, u32Value);
					callSig = MetaData_GetBlob(pCallSig->signature, NULL);
					callConv = MetaData_DecodeSigEntry(&callSig);
					numCallParams = MetaData_DecodeSigEntry(&callSig);
					pCallRetType = Type_GetTypeFromSig(pMetaData, &callSig, pMethodDef->pParentType->ppClassTypeArgs, pMethodDef->ppMethodTypeArgs);
					PopStackTypeDontCare(); // the function pointer
					// An instance signature (HASTHIS, without EXPLICITTHIS) has `this` pushed first too
					PopStackTypeMulti(numCallParams + (((callConv & SIG_METHODDEF_HASTHIS) && !(callConv & 0x40)) ? 1 : 0));
					PushOp(JIT_CALLI);
					if (pCallRetType != NULL) {
						MetaData_Fill_TypeDef(pCallRetType, NULL, NULL);
						PushStackType(pCallRetType);
					}
				}
				break;

			case CIL_JMP:
				// jmp <method>: transfer to another method, passing this method's own arguments. Done as
				// the equivalent "ldarg 0..n-1; call <method>; ret". The call translation reads the method
				// token itself, so it is deliberately not consumed here; jmpPending makes it end in a ret.
				PushOp(JIT_JMP_COPYARGS);
				for (i=0; i<pMethodDef->numberOfParameters; i++) {
					PushStackType(pMethodDef->pParams[i].pTypeDef);
				}
				op = CIL_CALL;
				u32Value2 = 0;
				jmpPending = 1;
				goto cilCallVirtConstrained;

			case CIL_UNBOX:
				{
					tMD_TypeDef *pUnboxType;

					PopStackTypeDontCare(); // the boxed object
					u32Value = GetUnalignedU32(pCIL, &cilOfs);
					pUnboxType = MetaData_GetTypeDefFromDefRefOrSpec(pMethodDef->pMetaData, u32Value, pMethodDef->pParentType->ppClassTypeArgs, pMethodDef->ppMethodTypeArgs);
					if (!pUnboxType->isValueType || pUnboxType->pGenericDefinition == types[TYPE_SYSTEM_NULLABLE]) {
						Crash("JITit(): unbox needs a (non-Nullable) value type");
					}
					MetaData_Fill_TypeDef(pUnboxType, NULL, NULL);
					PushOp(JIT_UNBOX);
					PushPTR(pUnboxType);
					PushStackType(types[TYPE_SYSTEM_INTPTR]);
				}
				break;

			case CIL_CKFINITE:
				pStackType = PopStackType();
				if (pStackType->stackType == EVALSTACK_F32) {
					PushOp(JIT_CKFINITE_F32);
				} else if (pStackType->stackType == EVALSTACK_F64) {
					PushOp(JIT_CKFINITE_F64);
				} else {
					Crash("JITit(): ckfinite needs a floating-point value, not stack type %d", pStackType->stackType);
				}
				PushStackType(pStackType);
				break;

			case CIL_CPOBJ:
				{
					tMD_TypeDef *pCpTypeDef;

					PopStackTypeMulti(2); // destination and source addresses
					u32Value = GetUnalignedU32(pCIL, &cilOfs);
					pCpTypeDef = MetaData_GetTypeDefFromDefRefOrSpec(pMethodDef->pMetaData, u32Value, pMethodDef->pParentType->ppClassTypeArgs, pMethodDef->ppMethodTypeArgs);
					MetaData_Fill_TypeDef(pCpTypeDef, NULL, NULL);
					PushOp(JIT_COPYOBJECT);
					PushPTR(pCpTypeDef);
				}
				break;

			case CIL_LDNULL:
				PushOp(JIT_LOAD_NULL);
				PushStackType(types[TYPE_SYSTEM_OBJECT]);
				break;

			case CIL_DUP:
				pStackType = PopStackType();
				PushStackType(pStackType);
				PushStackType(pStackType);
				switch (pStackType->stackSize) {
				case 4:
					PushOp(JIT_DUP_4);
					break;
				case 8:
					PushOp(JIT_DUP_8);
					break;
				default:
					PushOpParam(JIT_DUP_GENERAL, pStackType->stackSize);
					break;
				}
				break;

			case CIL_POP:
				pStackType = PopStackType();
				if (pStackType->stackSize == 4) {
					PushOp(JIT_POP_4);
				} else {
					PushOpParam(JIT_POP, pStackType->stackSize);
				}
				break;

			case CIL_LDC_I4_M1:
			case CIL_LDC_I4_0:
			case CIL_LDC_I4_1:
			case CIL_LDC_I4_2:
			case CIL_LDC_I4_3:
			case CIL_LDC_I4_4:
			case CIL_LDC_I4_5:
			case CIL_LDC_I4_6:
			case CIL_LDC_I4_7:
			case CIL_LDC_I4_8:
				i32Value = (I8)op - (I8)CIL_LDC_I4_0;
				goto cilLdcI4;

			case CIL_LDC_I4_S:
				i32Value = (I8)pCIL[cilOfs++];
				goto cilLdcI4;

			case CIL_LDC_I4:
				i32Value = (I32)GetUnalignedU32(pCIL, &cilOfs);
cilLdcI4:
				if (i32Value >= -1 && i32Value <= 2) {
					PushOp(JIT_LOAD_I4_0 + i32Value);
				} else {
					PushOp(JIT_LOAD_I32);
					PushI32(i32Value);
				}
				PushStackType(types[TYPE_SYSTEM_INT32]);
				break;

			case CIL_LDC_I8:
				PushOp(JIT_LOAD_I64);
				{
					U64 lo = GetUnalignedU32(pCIL, &cilOfs);
					U64 hi = GetUnalignedU32(pCIL, &cilOfs);
					PushU64((hi << 32) | lo);
				}
				PushStackType(types[TYPE_SYSTEM_INT64]);
				break;

			case CIL_LDC_R4:
				convFloat.u32 = GetUnalignedU32(pCIL, &cilOfs);
				PushStackType(types[TYPE_SYSTEM_SINGLE]);
				PushOp(JIT_LOAD_F32);
				PushFloat(convFloat.f);
				break;

			case CIL_LDC_R8:
				convDouble.u32.a = GetUnalignedU32(pCIL, &cilOfs);
				convDouble.u32.b = GetUnalignedU32(pCIL, &cilOfs);
				PushStackType(types[TYPE_SYSTEM_DOUBLE]);
				PushOp(JIT_LOAD_F64);
				PushDouble(convDouble.d);
				break;

			case CIL_LDARG_0:
			case CIL_LDARG_1:
			case CIL_LDARG_2:
			case CIL_LDARG_3:
				u32Value = op - CIL_LDARG_0;
				goto cilLdArg;

			case CIL_LDARG_S:
				u32Value = pCIL[cilOfs++];
cilLdArg:
				pStackType = pMethodDef->pParams[u32Value].pTypeDef;
				ofs = pMethodDef->pParams[u32Value].offset;
				if (pStackType->stackSize == 4 && ofs < 32) {
					PushOp(JIT_LOADPARAMLOCAL_0 + (ofs >> 2));
				} else {
					PushOpParam(JIT_LOADPARAMLOCAL_TYPEID + pStackType->stackType, ofs);
					// if it's a valuetype then push the TypeDef of it afterwards
					if (pStackType->stackType == EVALSTACK_VALUETYPE) {
						PushPTR(pStackType);
					}
				}
				PushStackType(pStackType);
				break;

			case CIL_LDARGA_S:
				// Get the argument number to load the address of
				u32Value = pCIL[cilOfs++];
cilLdArgA:
				PushOpParam(JIT_LOAD_PARAMLOCAL_ADDR, pMethodDef->pParams[u32Value].offset);
				PushStackType(types[TYPE_SYSTEM_INTPTR]);
				break;

			case CIL_STARG_S:
				// Get the argument number to store the arg of
				u32Value = pCIL[cilOfs++];
cilStArg:
				pStackType = PopStackType();
				ofs = pMethodDef->pParams[u32Value].offset;
				if (pStackType->stackSize == 4 && ofs < 32) {
					PushOp(JIT_STOREPARAMLOCAL_0 + (ofs >> 2));
				} else {
					PushOpParam(JIT_STOREPARAMLOCAL_TYPEID + pStackType->stackType, ofs);
					// if it's a valuetype then push the TypeDef of it afterwards
					if (pStackType->stackType == EVALSTACK_VALUETYPE) {
						PushPTR(pStackType);
					}
				}
				break;

			case CIL_LDLOC_0:
			case CIL_LDLOC_1:
			case CIL_LDLOC_2:
			case CIL_LDLOC_3:
				// Push opcode and offset into locals memory
				u32Value = op - CIL_LDLOC_0;
				goto cilLdLoc;

			case CIL_LDLOC_S:
				// Push opcode and offset into locals memory
				u32Value = pCIL[cilOfs++];
cilLdLoc:
				pStackType = pLocals[u32Value].pTypeDef;
				ofs = pMethodDef->parameterStackSize + pLocals[u32Value].offset;
				if (pStackType->stackSize == 4 && ofs < 32) {
					PushOp(JIT_LOADPARAMLOCAL_0 + (ofs >> 2));
				} else {
					PushOpParam(JIT_LOADPARAMLOCAL_TYPEID + pStackType->stackType, ofs);
					// if it's a valuetype then push the TypeDef of it afterwards
					if (pStackType->stackType == EVALSTACK_VALUETYPE) {
						PushPTR(pStackType);
					}
				}
				PushStackType(pStackType);
				break;

			case CIL_STLOC_0:
			case CIL_STLOC_1:
			case CIL_STLOC_2:
			case CIL_STLOC_3:
				u32Value = op - CIL_STLOC_0;
				goto cilStLoc;

			case CIL_STLOC_S:
				u32Value = pCIL[cilOfs++];
cilStLoc:
				pStackType = PopStackType();
				ofs = pMethodDef->parameterStackSize + pLocals[u32Value].offset;
				if (pStackType->stackSize == 4 && ofs < 32) {
					PushOp(JIT_STOREPARAMLOCAL_0 + (ofs >> 2));
				} else {
					PushOpParam(JIT_STOREPARAMLOCAL_TYPEID + pStackType->stackType, ofs);
					// if it's a valuetype then push the TypeDef of it afterwards
					if (pStackType->stackType == EVALSTACK_VALUETYPE) {
						PushPTR(pStackType);
					}
				}
				break;

			case CIL_LDLOCA_S:
				// Get the local number to load the address of
				u32Value = pCIL[cilOfs++];
cilLdLocA:
				PushOpParam(JIT_LOAD_PARAMLOCAL_ADDR, pMethodDef->parameterStackSize + pLocals[u32Value].offset);
				PushStackType(types[TYPE_SYSTEM_INTPTR]);
				break;

			case CIL_LDIND_I1:
				u32Value = TYPE_SYSTEM_SBYTE;
				goto cilLdInd;
			case CIL_LDIND_U1:
				u32Value = TYPE_SYSTEM_BYTE;
				goto cilLdInd;
			case CIL_LDIND_I2:
				u32Value = TYPE_SYSTEM_INT16;
				goto cilLdInd;
			case CIL_LDIND_U2:
				u32Value = TYPE_SYSTEM_UINT16;
				goto cilLdInd;
			case CIL_LDIND_I4:
				u32Value = TYPE_SYSTEM_INT32;
				goto cilLdInd;
			case CIL_LDIND_U4:
				u32Value = TYPE_SYSTEM_UINT32;
				goto cilLdInd;
			case CIL_LDIND_I8:
				u32Value = TYPE_SYSTEM_INT64;
				goto cilLdInd;
			case CIL_LDIND_R4:
				u32Value = TYPE_SYSTEM_SINGLE;
				goto cilLdInd;
			case CIL_LDIND_R8:
				u32Value = TYPE_SYSTEM_DOUBLE;
				goto cilLdInd;
			case CIL_LDIND_REF:
				u32Value = TYPE_SYSTEM_OBJECT;
				goto cilLdInd;
			case CIL_LDIND_I:
				u32Value = TYPE_SYSTEM_INTPTR;
cilLdInd:
				PopStackTypeDontCare(); // don't care what it is
				PushOp(JIT_LOADINDIRECT_I8 + (op - CIL_LDIND_I1));
				PushStackType(types[u32Value]);
				break;

			case CIL_STIND_I: // native int: pointer-sized, same as a reference
				PopStackTypeMulti(2); // Don't care what they are
				PushOp(JIT_STOREINDIRECT_REF);
				break;

			case CIL_STIND_REF:
			case CIL_STIND_I1:
			case CIL_STIND_I2:
			case CIL_STIND_I4:
			case CIL_STIND_I8:
			case CIL_STIND_R4:
			case CIL_STIND_R8:
				PopStackTypeMulti(2); // Don't care what they are
				PushOp(JIT_STOREINDIRECT_REF + (op - CIL_STIND_REF));
				break;

			case CIL_RET:
				PushOp(JIT_RETURN);
				RestoreTypeStack(&typeStack, ppTypeStacks[cilOfs]);
				break;

			case CIL_CALL:
			case CIL_CALLVIRT:
				{
					tMD_MethodDef *pCallMethod;
					tMD_TypeDef *pBoxCallType;
					const tFFIEntry *pFfiEntry;
					U32 derefRefType;
					U8 dynamicallyBoxReturnValue;

					u32Value2 = 0;

cilCallVirtConstrained:
					pBoxCallType = NULL;
					pFfiEntry = NULL;          // (reset here for the same reason as the rest)
					derefRefType = 0;
					// Must be reset here, not at the declaration: the CIL_CONSTRAINED prefix
					// jumps to the label above, past any initialiser on the declaration, which
					// left this indeterminate on that path.
					dynamicallyBoxReturnValue = 0;

					u32Value = GetUnalignedU32(pCIL, &cilOfs);
					pCallMethod = MetaData_GetMethodDefFromDefRefOrSpec(pMetaData, u32Value, pMethodDef->pParentType->ppClassTypeArgs, pMethodDef->ppMethodTypeArgs);
					if (pCallMethod->isFilled == 0) {
						tMD_TypeDef *pTypeDef;
						
						pTypeDef = MetaData_GetTypeDefFromMethodDef(pCallMethod);
						MetaData_Fill_TypeDef(pTypeDef, NULL, NULL);
					}

					if (u32Value2 != 0) {
						// There is a 'constrained' prefix
						tMD_TypeDef *pConstrainedType;

						pConstrainedType = MetaData_GetTypeDefFromDefRefOrSpec(pMetaData, u32Value2, pMethodDef->pParentType->ppClassTypeArgs, pMethodDef->ppMethodTypeArgs);
						if (TYPE_ISINTERFACE(pCallMethod->pParentType)) {
							u32Value2 = 0xffffffff;
							// Find the interface that we're dealing with
							for (i=0; i<pConstrainedType->numInterfaces; i++) {
								if (pConstrainedType->pInterfaceMaps[i].pInterface == pCallMethod->pParentType) {
									u32Value2 = pConstrainedType->pInterfaceMaps[i].pVTableLookup[pCallMethod->vTableOfs];
									break;
								}
							}
							Assert(u32Value2 != 0xffffffff);
							if (pConstrainedType->pVTable[u32Value2]->pParentType == pConstrainedType) {
								// This method is implemented on this class, so make it a normal CALL op
								op = CIL_CALL;
								pCallMethod = pConstrainedType->pVTable[u32Value2];
							}
						} else {
							if (pConstrainedType->isValueType) {
								tMD_MethodDef *pImplMethod;
								// If pConstraintedType directly implements the call then don't do anything
								// otherwise the 'this' pointer must be boxed (BoxedCall)
								pImplMethod = pConstrainedType->pVTable[pCallMethod->vTableOfs];
								if (pImplMethod->pParentType == pConstrainedType) {
									op = CIL_CALL;
									pCallMethod = pImplMethod;
								} else {
									pBoxCallType = pConstrainedType;
								}
							} else {
								// Reference-type, so dereference the PTR to 'this' and use that for the 'this' for the call.
								derefRefType = 1;
							}
						}
					}

					// Pop stack type for each argument. Don't actually care what these are,
					// except the last one which will be the 'this' object type of a non-static method
					//printf("Call %s() - popping %d stack args\n", pCallMethod->name, pCallMethod->numberOfParameters);
					for (i=0; i<pCallMethod->numberOfParameters; i++) {
						pStackType = PopStackType();
					}
					// the stack type of the 'this' object will now be in stackType (if there is one)
					if (METHOD_ISSTATIC(pCallMethod)) {
						pStackType = types[TYPE_SYSTEM_OBJECT];
					}
					MetaData_Fill_TypeDef(pStackType, NULL, NULL);
					// A DllImport of a function named in the FFI manifest is a call to the function itself, on the evaluation stack: no frame
					if ((pCallMethod->flags & METHODATTRIBUTES_PINVOKEIMPL) && FFI_Count() > 0) {
						tMD_ImplMap *pFfiImpl = MetaData_GetImplMap(pCallMethod->pMetaData, pCallMethod->tableIndex);
						pFfiEntry = FFI_Find(MetaData_GetModuleRefName(pCallMethod->pMetaData, pFfiImpl->importScope), pFfiImpl->importName);
						if (pFfiEntry != NULL) {
							const char *pBad = FFI_CheckSignature(pFfiEntry, pCallMethod);
							if (pBad != NULL) { Crash("FFI manifest and DllImport disagree: %s", pBad); }
						}
					}
					if (pFfiEntry != NULL) {
						PushOp(JIT_FFI_CALL);
					} else if (TYPE_ISINTERFACE(pCallMethod->pParentType) && op == CIL_CALLVIRT) {
						PushOp(JIT_CALL_INTERFACE);
					} else if (pCallMethod->pParentType->pParent == types[TYPE_SYSTEM_MULTICASTDELEGATE]) {
						PushOp(JIT_INVOKE_DELEGATE);
					} else if (pCallMethod->pParentType == types[TYPE_SYSTEM_REFLECTION_METHODBASE] && strcmp(pCallMethod->name, "Invoke") == 0) {
						PushOp(JIT_INVOKE_SYSTEM_REFLECTION_METHODBASE);
						dynamicallyBoxReturnValue = 1;
					} else {
						switch (pStackType->stackType)
						{
						case EVALSTACK_INTNATIVE: // a native-int `this` is called like a reference: both are pointer-sized
						case EVALSTACK_O:
							if (derefRefType) {
								PushOp(JIT_DEREF_CALLVIRT);
							} else {
								if (pBoxCallType != NULL) {
									PushOp(JIT_BOX_CALLVIRT);
									PushPTR(pBoxCallType);
								} else {
									PushOp((op == CIL_CALL)?JIT_CALL_O:JIT_CALLVIRT_O);
								}
							}
							break;
						case EVALSTACK_PTR:
						case EVALSTACK_VALUETYPE:
							if (derefRefType) {
								PushOp(JIT_DEREF_CALLVIRT);
							} else if (pBoxCallType != NULL) {
								PushOp(JIT_BOX_CALLVIRT);
								PushPTR(pBoxCallType);
							} else {
								PushOp(JIT_CALL_PTR);
							}
							break;
						default:
							Crash("JITit(): Cannot CALL or CALLVIRT with stack type: %d", pStackType->stackType);
						}
					}
					if (pFfiEntry != NULL) {
						PushPTR(pFfiEntry);
					} else {
						PushPTR(pCallMethod);
					}

					if (pCallMethod->pReturnType != NULL) {
						PushStackType(pCallMethod->pReturnType);
					}

					if (dynamicallyBoxReturnValue) {
						PushOp(JIT_REFLECTION_DYNAMICALLY_BOX_RETURN_VALUE);
					}

					if (jmpPending) {
						// the "ret" half of a jmp
						jmpPending = 0;
						PushOp(JIT_RETURN);
						RestoreTypeStack(&typeStack, ppTypeStacks[cilOfs]);
					}
				}
				break;

			case CIL_BR_S: // unconditional branch
				u32Value = (I8)pCIL[cilOfs++];
				goto cilBr;

			case CIL_BR:
				u32Value = GetUnalignedU32(pCIL, &cilOfs);
cilBr:
				// Put a temporary CIL offset value into the JITted code. This will be updated later
				u32Value = cilOfs + (I32)u32Value;
				MayCopyTypeStack();
				PushOp(JIT_BRANCH);
				PushBranch();
				PushU32(u32Value);
				// Restore the stack state
				RestoreTypeStack(&typeStack, ppTypeStacks[cilOfs]);
				break;

			case CIL_SWITCH:
				// This is the int containing the switch value. Don't care what it is.
				PopStackTypeDontCare();
				// The number of switch jump targets
				i32Value = (I32)GetUnalignedU32(pCIL, &cilOfs);
				// Set up the offset from which the jump offsets are calculated
				u32Value2 = cilOfs + (i32Value << 2);
				PushOpParam(JIT_SWITCH, i32Value);
				for (i=0; i<(U32)i32Value; i++) {
					// A jump target
					u32Value = u32Value2 + (I32)GetUnalignedU32(pCIL, &cilOfs);
					PushBranch();
					MayCopyTypeStack();
					// Push the jump target.
					// It is needed to allow the branch offset to be correctly updated later.
					PushU32(u32Value);
				}
				break;

			case CIL_BRFALSE_S:
			case CIL_BRTRUE_S:
				u32Value = (I8)pCIL[cilOfs++];
				u32Value2 = JIT_BRANCH_FALSE + (op - CIL_BRFALSE_S);
				goto cilBrFalseTrue;

			case CIL_BRFALSE:
			case CIL_BRTRUE:
				u32Value = GetUnalignedU32(pCIL, &cilOfs);
				u32Value2 = JIT_BRANCH_FALSE + (op - CIL_BRFALSE);
cilBrFalseTrue:
				pStackType = PopStackType();
				// Put a temporary CIL offset value into the JITted code. This will be updated later
				u32Value = cilOfs + (I32)u32Value;
				MayCopyTypeStack();
				if (sizeof(void*) > 4 && pStackType->stackSize == 8) {
					// an 8-byte operand (a reference on a 64-bit target): test all of it, and pop all of it
					PushOp(u32Value2 - JIT_BRANCH_FALSE + JIT_BRANCH_FALSE_PTR);
				} else {
					PushOp(u32Value2);
				}
				PushBranch();
				PushU32(u32Value);
				break;

			case CIL_BEQ_S:
			case CIL_BGE_S:
			case CIL_BGT_S:
			case CIL_BLE_S:
			case CIL_BLT_S:
			case CIL_BNE_UN_S:
			case CIL_BGE_UN_S:
			case CIL_BGT_UN_S:
			case CIL_BLE_UN_S:
			case CIL_BLT_UN_S:
				u32Value = (I8)pCIL[cilOfs++];
				u32Value2 = CIL_BEQ_S;
				goto cilBrCond;

			case CIL_BEQ:
			case CIL_BGE:
			case CIL_BGT:
			case CIL_BLE:
			case CIL_BLT:
			case CIL_BNE_UN:
			case CIL_BGE_UN:
			case CIL_BGT_UN:
			case CIL_BLE_UN:
			case CIL_BLT_UN:
				u32Value = GetUnalignedU32(pCIL, &cilOfs);
				u32Value2 = CIL_BEQ;
cilBrCond:
				pTypeB = PopStackType();
				pTypeA = PopStackType();
				u32Value = cilOfs + (I32)u32Value;
				MayCopyTypeStack();
				if (pTypeA->stackType == EVALSTACK_O && pTypeB->stackType == EVALSTACK_O && sizeof(void*) > 4) {
					// references are 8 bytes here, so compare them as 64-bit values
					PushOp(JIT_BEQ_I64I64 + (op - u32Value2));
				} else if ((pTypeA->stackType == EVALSTACK_INT32 && pTypeB->stackType == EVALSTACK_INT32) ||
					(pTypeA->stackType == EVALSTACK_O && pTypeB->stackType == EVALSTACK_O)) {
					PushOp(JIT_BEQ_I32I32 + (op - u32Value2));
				} else if (pTypeA->stackType == EVALSTACK_INT64 && pTypeB->stackType == EVALSTACK_INT64) {
					PushOp(JIT_BEQ_I64I64 + (op - u32Value2));
				} else if (pTypeA->stackType == EVALSTACK_F32 && pTypeB->stackType == EVALSTACK_F32) {
					PushOp(JIT_BEQ_F32F32 + (op - u32Value2));
				} else if (pTypeA->stackType == EVALSTACK_F64 && pTypeB->stackType == EVALSTACK_F64) {
					PushOp(JIT_BEQ_F64F64 + (op - u32Value2));
				} else {
					Crash("JITit(): Cannot perform conditional branch on stack types: %d and %d", pTypeA->stackType, pTypeB->stackType);
				}
				PushBranch();
				PushU32(u32Value);
				break;

			case CIL_ADD_OVF:
			case CIL_ADD_OVF_UN:
			case CIL_MUL_OVF:
			case CIL_MUL_OVF_UN:
			case CIL_SUB_OVF:
			case CIL_SUB_OVF_UN:
				u32Value = (CIL_ADD_OVF - CIL_ADD) + (JIT_ADD_I32I32 - JIT_ADD_OVF_I32I32);
				goto cilBinaryArithOp;
			case CIL_ADD:
			case CIL_SUB:
			case CIL_MUL:
			case CIL_DIV:
			case CIL_DIV_UN:
			case CIL_REM:
			case CIL_REM_UN:
			case CIL_AND:
			case CIL_OR:
			case CIL_XOR:
				u32Value = 0;
cilBinaryArithOp:
				pTypeB = PopStackType();
				pTypeA = PopStackType();
				if (pTypeA->stackType == EVALSTACK_INT32 && pTypeB->stackType == EVALSTACK_INT32) {
					PushOp(JIT_ADD_I32I32 + (op - CIL_ADD) - u32Value);
					PushStackType(types[TYPE_SYSTEM_INT32]);
				} else if (pTypeA->stackType == EVALSTACK_INT64 && pTypeB->stackType == EVALSTACK_INT64) {
					PushOp(JIT_ADD_I64I64 + (op - CIL_ADD) - u32Value);
					PushStackType(types[TYPE_SYSTEM_INT64]);
				} else if (pTypeA->stackType == EVALSTACK_F32 && pTypeB->stackType == EVALSTACK_F32) {
					PushOp(JIT_ADD_F32F32 + (op - CIL_ADD) - u32Value);
					PushStackType(pTypeA);
				} else if (pTypeA->stackType == EVALSTACK_F64 && pTypeB->stackType == EVALSTACK_F64) {
					PushOp(JIT_ADD_F64F64 + (op - CIL_ADD) - u32Value);
					PushStackType(pTypeA);
				} else {
					Crash("JITit(): Cannot perform binary numeric operand on stack types: %d and %d", pTypeA->stackType, pTypeB->stackType);
				}
				break;

			case CIL_NEG:
			case CIL_NOT:
				pTypeA = PopStackType();
				if (pTypeA->stackType == EVALSTACK_INT32) {
					PushOp(JIT_NEG_I32 + (op - CIL_NEG));
					PushStackType(types[TYPE_SYSTEM_INT32]);
				} else if (pTypeA->stackType == EVALSTACK_INT64) {
					PushOp(JIT_NEG_I64 + (op - CIL_NEG));
					PushStackType(types[TYPE_SYSTEM_INT64]);
				} else if (op == CIL_NEG && pTypeA->stackType == EVALSTACK_F32) {
					PushOp(JIT_NEG_F32);
					PushStackType(types[TYPE_SYSTEM_SINGLE]);
				} else if (op == CIL_NEG && pTypeA->stackType == EVALSTACK_F64) {
					PushOp(JIT_NEG_F64);
					PushStackType(types[TYPE_SYSTEM_DOUBLE]);
				} else {
					Crash("JITit(): Cannot perform unary operand on stack types: %d", pTypeA->stackType);
				}
				break;

			case CIL_SHL:
			case CIL_SHR:
			case CIL_SHR_UN:
				PopStackTypeDontCare(); // Don't care about the shift amount
				pTypeA = PopStackType(); // Do care about the value to shift
				if (pTypeA->stackType == EVALSTACK_INT32) {
					PushOp(JIT_SHL_I32 - CIL_SHL + op);
					PushStackType(types[TYPE_SYSTEM_INT32]);
				} else if (pTypeA->stackType == EVALSTACK_INT64) {
					PushOp(JIT_SHL_I64 - CIL_SHL + op);
					PushStackType(types[TYPE_SYSTEM_INT64]);
				} else {
					Crash("JITit(): Cannot perform shift operation on type: %s", pTypeA->name);
				}
				break;

				// Conversion operations
				{
					U32 toType;
					U32 toBitCount;
					U32 convOpOffset;
			case CIL_CONV_I1:
			case CIL_CONV_OVF_I1: // Fix this later - will never overflow
			case CIL_CONV_OVF_I1_UN: // Fix this later - will never overflow
				toBitCount = 8;
				toType = TYPE_SYSTEM_SBYTE;
				goto cilConvInt32;
			case CIL_CONV_I2:
			case CIL_CONV_OVF_I2: // Fix this later - will never overflow
			case CIL_CONV_OVF_I2_UN: // Fix this later - will never overflow
				toBitCount = 16;
				toType = TYPE_SYSTEM_INT16;
				goto cilConvInt32;
			case CIL_CONV_I:
			case CIL_CONV_OVF_I:
			case CIL_CONV_OVF_I_UN:
				// A native int is handled on the evaluation stack as an integer of pointer width: an
				// int64 on a 64-bit target (so arithmetic on it uses the 64-bit ops and it can be stored
				// through a pointer-sized slot), an int32 on a 32-bit one.
				if (sizeof(void*) > 4) {
					toType = TYPE_SYSTEM_INT64;
					convOpOffset = JIT_CONV_OFFSET_I64;
					goto cilConv;
				}
				// fall through
			case CIL_CONV_I4:
			case CIL_CONV_OVF_I4: // Fix this later - will never overflow
			case CIL_CONV_OVF_I4_UN: // Fix this later - will never overflow
				toBitCount = 32;
				toType = TYPE_SYSTEM_INT32;
cilConvInt32:
				convOpOffset = JIT_CONV_OFFSET_I32;
				goto cilConv;
			case CIL_CONV_U1:
			case CIL_CONV_OVF_U1: // Fix this later - will never overflow
			case CIL_CONV_OVF_U1_UN: // Fix this later - will never overflow
				toBitCount = 8;
				toType = TYPE_SYSTEM_BYTE;
				goto cilConvUInt32;
			case CIL_CONV_U2:
			case CIL_CONV_OVF_U2: // Fix this later - will never overflow
			case CIL_CONV_OVF_U2_UN: // Fix this later - will never overflow
				toBitCount = 16;
				toType = TYPE_SYSTEM_UINT16;
				goto cilConvUInt32;
			case CIL_CONV_U:
			case CIL_CONV_OVF_U:
			case CIL_CONV_OVF_U_UN:
				// (see conv.i above: pointer width, zero-extended)
				if (sizeof(void*) > 4) {
					toType = TYPE_SYSTEM_UINT64;
					convOpOffset = JIT_CONV_OFFSET_U64;
					goto cilConv;
				}
				// fall through
			case CIL_CONV_U4:
			case CIL_CONV_OVF_U4: // Fix this later - will never overflow
			case CIL_CONV_OVF_U4_UN: // Fix this later - will never overflow
				toBitCount = 32;
				toType = TYPE_SYSTEM_UINT32;
cilConvUInt32:
				convOpOffset = JIT_CONV_OFFSET_U32;
				goto cilConv;
			case CIL_CONV_I8:
			case CIL_CONV_OVF_I8: // Fix this later - will never overflow
			case CIL_CONV_OVF_I8_UN: // Fix this later - will never overflow
				toType = TYPE_SYSTEM_INT64;
				convOpOffset = JIT_CONV_OFFSET_I64;
				goto cilConv;
			case CIL_CONV_U8:
			case CIL_CONV_OVF_U8: // Fix this later - will never overflow
			case CIL_CONV_OVF_U8_UN: // Fix this later - will never overflow
				toType = TYPE_SYSTEM_UINT64;
				convOpOffset = JIT_CONV_OFFSET_U64;
				goto cilConv;
			case CIL_CONV_R4:
				toType = TYPE_SYSTEM_SINGLE;
				convOpOffset = JIT_CONV_OFFSET_R32;
				goto cilConv;
			case CIL_CONV_R8:
			case CIL_CONV_R_UN:
				toType = TYPE_SYSTEM_DOUBLE;
				convOpOffset = JIT_CONV_OFFSET_R64;
				goto cilConv;
cilConv:
				pStackType = PopStackType();
				{
					U32 opCodeBase;
					U32 useParam = 0, param;
					{
						// conv.ovf.*: check the value fits first; the conversion op that follows
						// then just narrows/widens it. (These used to be aliased to the unchecked
						// conversions and silently wrapped.)
						U32 ovfUnsigned;
						I32 ovfTo = ConvOvfTarget(op, &ovfUnsigned);
						if (ovfTo >= 0) {
							U32 ovfFrom;
							switch (pStackType->stackType) {
							case EVALSTACK_PTR: // pointer-sized: a 64-bit value on a 64-bit target
								ovfFrom = (sizeof(void*) > 4) ? (ovfUnsigned ? 3 : 2) : (ovfUnsigned ? 1 : 0);
								break;
							case EVALSTACK_INT64: ovfFrom = ovfUnsigned ? 3 : 2; break;
							case EVALSTACK_F32: ovfFrom = 4; break;
							case EVALSTACK_F64: ovfFrom = 5; break;
							default: ovfFrom = ovfUnsigned ? 1 : 0; break; // INT32 and native int
							}
							PushOpParam(JIT_CONV_OVF_CHECK, ovfFrom | ((U32)ovfTo << 8));
						}
					}
					// This is the types that the conversion is from.
					switch (pStackType->stackType) {
					case EVALSTACK_PTR:
						if (sizeof(void*) > 4) {
							// a pointer is 8 bytes here, so convert from it as from a 64-bit value
							opCodeBase = ConvSourceIsUnsigned(op)?JIT_CONV_FROM_U64:JIT_CONV_FROM_I64;
							break;
						}
						// fall through: on a 32-bit target a pointer is a 32-bit value
					case EVALSTACK_INT32:
						opCodeBase = ConvSourceIsUnsigned(op)?JIT_CONV_FROM_U32:JIT_CONV_FROM_I32;
						break;
					case EVALSTACK_INT64:
						opCodeBase = ConvSourceIsUnsigned(op)?JIT_CONV_FROM_U64:JIT_CONV_FROM_I64;
						break;
					case EVALSTACK_F64:
						opCodeBase = JIT_CONV_FROM_R64;
						break;
					case EVALSTACK_F32:
						opCodeBase = JIT_CONV_FROM_R32;
						break;
					default:
						Crash("JITit() Conv cannot handle stack type %d", pStackType->stackType);
					}
					// This is the types that the conversion is to.
					switch (convOpOffset) {
					case JIT_CONV_OFFSET_I32:
						useParam = 1;
						param = 32 - toBitCount;
						break;
					case JIT_CONV_OFFSET_U32:
						useParam = 1;
						// Next line is really (1 << toBitCount) - 1
						// But it's done like this to work when toBitCount == 32
						param = (((1 << (toBitCount - 1)) - 1) << 1) + 1;
						break;
					case JIT_CONV_OFFSET_I64:
					case JIT_CONV_OFFSET_U64:
					case JIT_CONV_OFFSET_R32:
					case JIT_CONV_OFFSET_R64:
						break;
					default:
						Crash("JITit() Conv cannot handle convOpOffset %d", convOpOffset);
					}
					if ((convOpOffset == JIT_CONV_OFFSET_I64 || convOpOffset == JIT_CONV_OFFSET_U64) &&
							(opCodeBase == JIT_CONV_FROM_I64 || opCodeBase == JIT_CONV_FROM_U64)) {
						// 64 bits to 64 bits (including a pointer, here): the 8 bytes on the stack are already what the result is. There is no op
						// for it (they were "not used": IntPtr.ToInt64() could not be compiled on a 64-bit target).
					} else {
						PushOp(opCodeBase + convOpOffset);
						if (useParam) {
							PushU32(param);
						}
					}
				}
				PushStackType(types[toType]);
				break;
				}

#ifdef OLD_CONV
			case CIL_CONV_OVF_I1:
			case CIL_CONV_OVF_I2:
			case CIL_CONV_OVF_I4:
				u32Value = TYPE_SYSTEM_INT32;
				goto convOvf;
			case CIL_CONV_OVF_I8:
				u32Value = TYPE_SYSTEM_INT64;
				goto convOvf;
			case CIL_CONV_OVF_U1:
			case CIL_CONV_OVF_U2:
			case CIL_CONV_OVF_U4:
				u32Value = TYPE_SYSTEM_UINT32;
				goto convOvf;
			case CIL_CONV_OVF_U8:
				u32Value = TYPE_SYSTEM_UINT64;
convOvf:
				pStackType = PopStackType();
				PushOpParam(JIT_CONV_OVF_I1 + (op - CIL_CONV_OVF_I1), pStackType->stackType);
				PushStackType(types[u32Value]);
				break;

			case CIL_CONV_I1:
			case CIL_CONV_I2:
			case CIL_CONV_I4:
				u32Value = TYPE_SYSTEM_INT32;
				goto conv1;
			case CIL_CONV_I8:
				u32Value = TYPE_SYSTEM_INT64;
				goto conv1;
			case CIL_CONV_R4:
				u32Value = TYPE_SYSTEM_SINGLE;
				goto conv1;
			case CIL_CONV_R8:
				u32Value = TYPE_SYSTEM_DOUBLE;
				goto conv1;
			case CIL_CONV_U4:
				u32Value = TYPE_SYSTEM_UINT32;
				goto conv1;
			case CIL_CONV_U8:
				u32Value = TYPE_SYSTEM_UINT64;
conv1:
				pStackType = PopStackType();
				PushOpParam(JIT_CONV_I1 + (op - CIL_CONV_I1), pStackType->stackType);
				PushStackType(types[u32Value]);
				break;

			case CIL_CONV_U2:
			case CIL_CONV_U1:
				u32Value = TYPE_SYSTEM_UINT32;
				goto conv2;
			case CIL_CONV_I:
				u32Value = TYPE_SYSTEM_INT32; // Only on 32-bit
conv2:
				pStackType = PopStackType();
				PushOpParam(JIT_CONV_U2 + (op - CIL_CONV_U2), pStackType->stackType);
				PushStackType(types[u32Value]);
				break;

			case CIL_CONV_U:
				pStackType = PopStackType();
				PushOpParam(JIT_CONV_U_NATIVE, pStackType->stackType);
				PushStackType(types[TYPE_SYSTEM_UINTPTR]);
				break;
#endif

			case CIL_LDOBJ:
				{
					tMD_TypeDef *pTypeDef;

					PopStackTypeDontCare(); // Don't care what this is
					u32Value = GetUnalignedU32(pCIL, &cilOfs);
					pTypeDef = MetaData_GetTypeDefFromDefRefOrSpec(pMethodDef->pMetaData, u32Value, pMethodDef->pParentType->ppClassTypeArgs, pMethodDef->ppMethodTypeArgs);
					PushOp(JIT_LOADOBJECT);
					PushPTR(pTypeDef);
					PushStackType(pTypeDef);
				}
				break;

			case CIL_STOBJ:
				{
					tMD_TypeDef *pTypeDef;

					u32Value = GetUnalignedU32(pCIL, &cilOfs);
					pTypeDef = MetaData_GetTypeDefFromDefRefOrSpec(pMethodDef->pMetaData, u32Value, pMethodDef->pParentType->ppClassTypeArgs, pMethodDef->ppMethodTypeArgs);
					PopStackTypeMulti(2);
					if (pTypeDef->isValueType && pTypeDef->arrayElementSize != 4) {
						// If it's a value-type then do this
						PushOpParam(JIT_STORE_OBJECT_VALUETYPE, pTypeDef->arrayElementSize);
					} else if (pTypeDef->isValueType) {
						// A value-type with size 4 can use the plain 4-byte store (it executes faster).
						// (This used to share one op with reference types, which are only 4 bytes on a
						// 32-bit target.)
						PushOp(JIT_STOREINDIRECT_U32);
					} else {
						// A reference: pointer-sized
						PushOp(JIT_STOREINDIRECT_REF);
					}
					break;
				}

			case CIL_LDSTR:
				u32Value = GetUnalignedU32(pCIL, &cilOfs) & 0x00ffffff;
				PushOpParam(JIT_LOAD_STRING, u32Value);
				PushStackType(types[TYPE_SYSTEM_STRING]);
				break;

			case CIL_NEWOBJ:
				{
					tMD_MethodDef *pConstructorDef;

					u32Value = GetUnalignedU32(pCIL, &cilOfs);
					pConstructorDef = MetaData_GetMethodDefFromDefRefOrSpec(pMetaData, u32Value, pMethodDef->pParentType->ppClassTypeArgs, pMethodDef->ppMethodTypeArgs);
					if (pConstructorDef->isFilled == 0) {
						tMD_TypeDef *pTypeDef;

						pTypeDef = MetaData_GetTypeDefFromMethodDef(pConstructorDef);
						MetaData_Fill_TypeDef(pTypeDef, NULL, NULL);
					}
					if (pConstructorDef->pParentType->isValueType) {
						PushOp(JIT_NEWOBJECT_VALUETYPE);
					} else {
						PushOp(JIT_NEWOBJECT);
					}
					// -1 because the param count includes the 'this' parameter that is sent to the constructor
					PopStackTypeMulti(pConstructorDef->numberOfParameters - 1);
					PushPTR(pConstructorDef);
					PushStackType(pConstructorDef->pParentType);
				}
				break;

			case CIL_CASTCLASS:
				{
					tMD_TypeDef *pCastToType;

					PushOp(JIT_CAST_CLASS);
					u32Value = GetUnalignedU32(pCIL, &cilOfs);
					pCastToType = MetaData_GetTypeDefFromDefRefOrSpec(pMethodDef->pMetaData, u32Value, pMethodDef->pParentType->ppClassTypeArgs, pMethodDef->ppMethodTypeArgs);
					PushPTR(pCastToType);
				}
				break;

			case CIL_ISINST:
				{
					tMD_TypeDef *pIsInstanceOfType;

					PushOp(JIT_IS_INSTANCE);
					u32Value = GetUnalignedU32(pCIL, &cilOfs);
					pIsInstanceOfType = MetaData_GetTypeDefFromDefRefOrSpec(pMethodDef->pMetaData, u32Value, pMethodDef->pParentType->ppClassTypeArgs, pMethodDef->ppMethodTypeArgs);
					PushPTR(pIsInstanceOfType);
				}
				break;

			case CIL_NEWARR:
				{
					tMD_TypeDef *pTypeDef;

					u32Value = GetUnalignedU32(pCIL, &cilOfs);
					pTypeDef = MetaData_GetTypeDefFromDefRefOrSpec(pMethodDef->pMetaData, u32Value, pMethodDef->pParentType->ppClassTypeArgs, pMethodDef->ppMethodTypeArgs);
					NarrowWideIndexTop();
					PopStackTypeDontCare(); // Don't care what it is
					PushOp(JIT_NEW_VECTOR);
					MetaData_Fill_TypeDef(pTypeDef, NULL, NULL);
					pTypeDef = Type_GetArrayTypeDef(pTypeDef, pMethodDef->pParentType->ppClassTypeArgs, pMethodDef->ppMethodTypeArgs);
					PushPTR(pTypeDef);
					PushStackType(pTypeDef);
				}
				break;

			case CIL_LDLEN:
				PopStackTypeDontCare(); // Don't care what it is
				PushOp(JIT_LOAD_VECTOR_LEN);
				PushStackType(types[TYPE_SYSTEM_INT32]);
				break;

			case CIL_LDELEM_U1:
				// byte[] has 1-byte elements and bool[] 4-byte ones: use the array's type, if it is known, to choose
				NarrowWideIndexTop(); PopStackTypeMulti(2);
				{
					U32 elemSize = KnownArrayElementSize(typeStack.ppTypes[typeStack.ofs]);   // the array: popped, still there
					PushOp(elemSize == 1 ? JIT_LOAD_ELEMENT_U8_1 : (elemSize == 4 ? JIT_LOAD_ELEMENT_U8_4 : JIT_LOAD_ELEMENT_U8));
				}
				PushStackType(types[TYPE_SYSTEM_INT32]);
				break;
			case CIL_LDELEM_I1:
			case CIL_LDELEM_I2:
			case CIL_LDELEM_U2:
			case CIL_LDELEM_I4:
			case CIL_LDELEM_U4:
				NarrowWideIndexTop(); PopStackTypeMulti(2); // Don't care what any of these are
				PushOp(JIT_LOAD_ELEMENT_I8 + (op - CIL_LDELEM_I1));
				PushStackType(types[TYPE_SYSTEM_INT32]);
				break;

			case CIL_LDELEM_I8:
				NarrowWideIndexTop(); PopStackTypeMulti(2); // Don't care what any of these are
				PushOp(JIT_LOAD_ELEMENT_I64);
				PushStackType(types[TYPE_SYSTEM_INT64]);
				break;

			case CIL_LDELEM_R4:
				NarrowWideIndexTop(); PopStackTypeMulti(2); // Don't care what any of these are
				PushOp(JIT_LOAD_ELEMENT_R32);
				PushStackType(types[TYPE_SYSTEM_SINGLE]);
				break;

			case CIL_LDELEM_R8:
				NarrowWideIndexTop(); PopStackTypeMulti(2); // Don't care what any of these are
				PushOp(JIT_LOAD_ELEMENT_R64);
				PushStackType(types[TYPE_SYSTEM_DOUBLE]);
				break;

			case CIL_LDELEM_REF:
				NarrowWideIndexTop(); PopStackTypeMulti(2); // Don't care what any of these are
				PushOp(JIT_LOAD_ELEMENT_PTR);
				PushStackType(types[TYPE_SYSTEM_OBJECT]);
				break;

			case CIL_LDELEM_I: // native int: pointer-sized
				NarrowWideIndexTop(); PopStackTypeMulti(2); // Don't care what any of these are
				PushOp(JIT_LOAD_ELEMENT_PTR);
				PushStackType(types[TYPE_SYSTEM_INTPTR]);
				break;

			case CIL_LDELEM_ANY:
				u32Value = GetUnalignedU32(pCIL, &cilOfs);
				pStackType = (tMD_TypeDef*)MetaData_GetTypeDefFromDefRefOrSpec(pMetaData, u32Value, pMethodDef->pParentType->ppClassTypeArgs, pMethodDef->ppMethodTypeArgs);
				NarrowWideIndexTop(); PopStackTypeMulti(2); // Don't care what these are
				MetaData_Fill_TypeDef(pStackType, NULL, NULL);
				if (pStackType == types[TYPE_SYSTEM_SBYTE] || pStackType == types[TYPE_SYSTEM_INT16]) {
					// a signed element narrower than the stack slot has to be sign-extended, as ldelem.i1 and ldelem.i2 do (the general load
					// copied the element's bytes into a zeroed slot: -900 in a List<short> came back as 64636)
					PushOp(pStackType == types[TYPE_SYSTEM_SBYTE] ? JIT_LOAD_ELEMENT_I8 : JIT_LOAD_ELEMENT_I16);
				} else if (pStackType->arrayElementSize == 4 && pStackType->stackSize == 4) {
					// a 4-byte element (List<int>, List<float>): the token says so, and then it is the same as ldelem.i4 (which a native block does)
					PushOp(JIT_LOAD_ELEMENT_I32);
				} else {
					PushOpParam(JIT_LOAD_ELEMENT, pStackType->stackSize);
				}
				PushStackType(pStackType);
				break;

			case CIL_LDELEMA:
				{
					// the type token gives the element size, which the native blocks need to compute the address
					tMD_TypeDef *pElemType;
					u32Value = GetUnalignedU32(pCIL, &cilOfs);
					pElemType = MetaData_GetTypeDefFromDefRefOrSpec(pMetaData, u32Value, pMethodDef->pParentType->ppClassTypeArgs, pMethodDef->ppMethodTypeArgs);
					MetaData_Fill_TypeDef(pElemType, NULL, NULL);
					NarrowWideIndexTop(); PopStackTypeMulti(2); // Don't care what any of these are
					PushOpParam(JIT_LOAD_ELEMENT_ADDR_N, pElemType->arrayElementSize);
					PushStackType(types[TYPE_SYSTEM_INTPTR]);
				}
				break;

			case CIL_STELEM_I1:
				NarrowWideIndexBelowValue(); PopStackTypeMulti(3); // Don't care what any of these are
				{
					// byte[] and sbyte[] have 1-byte elements, bool[] 4-byte ones: choose from the array's type, if it is known
					U32 elemSize = KnownArrayElementSize(typeStack.ppTypes[typeStack.ofs]);
					PushOp(elemSize == 1 ? JIT_STORE_ELEMENT_I1 : (elemSize == 4 ? JIT_STORE_ELEMENT_I4 : JIT_STORE_ELEMENT_32));
				}
				break;
			case CIL_STELEM_I2:
				NarrowWideIndexBelowValue(); PopStackTypeMulti(3);
				PushOp(JIT_STORE_ELEMENT_I2);
				break;
			case CIL_STELEM_I4:
			case CIL_STELEM_R4:
				NarrowWideIndexBelowValue(); PopStackTypeMulti(3);
				PushOp(JIT_STORE_ELEMENT_I4);
				break;

			case CIL_STELEM_I:   // native int and references are pointer-sized
			case CIL_STELEM_REF:
				NarrowWideIndexBelowValue(); PopStackTypeMulti(3); // Don't care what any of these are
				PushOp(JIT_STORE_ELEMENT_PTR);
				break;

			case CIL_STELEM_I8:
			case CIL_STELEM_R8:
				NarrowWideIndexBelowValue(); PopStackTypeMulti(3); // Don't care what any of these are
				PushOp(JIT_STORE_ELEMENT_64);
				break;

			case CIL_STELEM_ANY:
				GetUnalignedU32(pCIL, &cilOfs); // Don't need this token, as the type stack will contain the same type
				NarrowWideIndexBelowValue();
				pStackType = PopStackType(); // This is the type to store
				PopStackTypeMulti(2); // Don't care what these are
				if (pStackType->stackSize == 4 && KnownArrayElementSize(typeStack.ppTypes[typeStack.ofs]) == 4) {
					PushOp(JIT_STORE_ELEMENT_I4);     // (the array's type, still on the type stack, says its elements are 4 bytes)
				} else {
					PushOpParam(JIT_STORE_ELEMENT, pStackType->stackSize);
				}
				break;

			case CIL_STFLD:
				{
					tMD_FieldDef *pFieldDef;

					// Get the stack type of the value to store
					pStackType = PopStackType();
					PushOp(JIT_STOREFIELD_TYPEID + pStackType->stackType);
					// Get the FieldRef or FieldDef of the field to store
					u32Value = GetUnalignedU32(pCIL, &cilOfs);
					pFieldDef = MetaData_GetFieldDefFromDefOrRef(pMethodDef->pMetaData, u32Value, pMethodDef->pParentType->ppClassTypeArgs, pMethodDef->ppMethodTypeArgs);
					PushPTR(pFieldDef);
					// Pop the object/valuetype on which to store the field. Don't care what it is
					PopStackTypeDontCare();
				}
				break;

			case CIL_LDFLD:
				{
					tMD_FieldDef *pFieldDef;

					// Get the FieldRef or FieldDef of the field to load
					u32Value = GetUnalignedU32(pCIL, &cilOfs);
					pFieldDef = MetaData_GetFieldDefFromDefOrRef(pMethodDef->pMetaData, u32Value, pMethodDef->pParentType->ppClassTypeArgs, pMethodDef->ppMethodTypeArgs);
					// Pop the object/valuetype on which to load the field.
					pStackType = PopStackType();
					if (pStackType->stackType == EVALSTACK_VALUETYPE) {
						PushOpParam(JIT_LOADFIELD_VALUETYPE, pStackType->stackSize);
						PushPTR(pFieldDef);
					} else {
						if (pFieldDef->memSize <= 4) {
							PushOp(JIT_LOADFIELD_4);
							PushU32(pFieldDef->memOffset);
						} else {
							PushOp(JIT_LOADFIELD);
							PushPTR(pFieldDef);
						}
					}
					// Push the stack type of the just-read field
					PushStackType(pFieldDef->pType);
				}
				break;

			case CIL_LDFLDA:
				{
					tMD_FieldDef *pFieldDef;
					tMD_TypeDef *pTypeDef;

					// Get the FieldRef or FieldDef of the field to load
					u32Value = GetUnalignedU32(pCIL, &cilOfs);
					pFieldDef = MetaData_GetFieldDefFromDefOrRef(pMethodDef->pMetaData, u32Value, pMethodDef->pParentType->ppClassTypeArgs, pMethodDef->ppMethodTypeArgs);
					// Sometimes, the type def will not have been filled, so ensure it's filled.
					pTypeDef = MetaData_GetTypeDefFromFieldDef(pFieldDef);
					MetaData_Fill_TypeDef(pTypeDef, NULL, NULL);
					PopStackTypeDontCare(); // Don't care what it is
					PushOpParam(JIT_LOAD_FIELD_ADDR, pFieldDef->memOffset);
					PushStackType(types[TYPE_SYSTEM_INTPTR]);
				}
				break;

			case CIL_STSFLD: // Store static field
				{
					tMD_FieldDef *pFieldDef;
					tMD_TypeDef *pTypeDef;

					// Get the FieldRef or FieldDef of the static field to store
					PopStackTypeDontCare(); // Don't care what it is
					u32Value = GetUnalignedU32(pCIL, &cilOfs);
					pFieldDef = MetaData_GetFieldDefFromDefOrRef(pMethodDef->pMetaData, u32Value, pMethodDef->pParentType->ppClassTypeArgs, pMethodDef->ppMethodTypeArgs);
					// Sometimes, the type def will not have been filled, so ensure it's filled.
					pTypeDef = MetaData_GetTypeDefFromFieldDef(pFieldDef);
					MetaData_Fill_TypeDef(pTypeDef, NULL, NULL);
					pStackType = pFieldDef->pType;
					PushOp(JIT_STORESTATICFIELD_TYPEID + pStackType->stackType);
					PushPTR(pFieldDef);
				}
				break;

			case CIL_LDSFLD: // Load static field
				{
					tMD_FieldDef *pFieldDef;
					tMD_TypeDef *pTypeDef;

					// Get the FieldRef or FieldDef of the static field to load
					u32Value = GetUnalignedU32(pCIL, &cilOfs);
					pFieldDef = MetaData_GetFieldDefFromDefOrRef(pMethodDef->pMetaData, u32Value, pMethodDef->pParentType->ppClassTypeArgs, pMethodDef->ppMethodTypeArgs);
					// Sometimes, the type def will not have been filled, so ensure it's filled.
					pTypeDef = MetaData_GetTypeDefFromFieldDef(pFieldDef);
					MetaData_Fill_TypeDef(pTypeDef, NULL, NULL);
					pStackType = pFieldDef->pType;
					PushOp(JIT_LOADSTATICFIELD_CHECKTYPEINIT_TYPEID + pStackType->stackType);
					PushPTR(pFieldDef);
					PushStackType(pStackType);
				}
				break;

			case CIL_LDSFLDA: // Load static field address
				{
					tMD_FieldDef *pFieldDef;
					tMD_TypeDef *pTypeDef;

					// Get the FieldRef or FieldDef of the field to load
					u32Value = GetUnalignedU32(pCIL, &cilOfs);
					pFieldDef = MetaData_GetFieldDefFromDefOrRef(pMethodDef->pMetaData, u32Value, pMethodDef->pParentType->ppClassTypeArgs, pMethodDef->ppMethodTypeArgs);
					// Sometimes, the type def will not have been filled, so ensure it's filled.
					pTypeDef = MetaData_GetTypeDefFromFieldDef(pFieldDef);
					MetaData_Fill_TypeDef(pTypeDef, NULL, NULL);
					PushOp(JIT_LOADSTATICFIELDADDRESS_CHECKTYPEINIT);
					PushPTR(pFieldDef);
					PushStackType(types[TYPE_SYSTEM_INTPTR]);
				}
				break;

			case CIL_BOX:
				{
					tMD_TypeDef *pTypeDef;

					pStackType = PopStackType();
					// Get the TypeDef(or Ref) token of the valuetype to box
					u32Value = GetUnalignedU32(pCIL, &cilOfs);
					pTypeDef = MetaData_GetTypeDefFromDefRefOrSpec(pMethodDef->pMetaData, u32Value, pMethodDef->pParentType->ppClassTypeArgs, pMethodDef->ppMethodTypeArgs);
					MetaData_Fill_TypeDef(pTypeDef, NULL, NULL);
					if (pTypeDef->pGenericDefinition == types[TYPE_SYSTEM_NULLABLE]) {
						// This is a nullable type, so special boxing code is needed.
						PushOp(JIT_BOX_NULLABLE);
						// Push the underlying type of the nullable type, and then the nullable type itself
						// (its size and where .Value sits in it are not simply "+4": they follow its layout)
						PushPTR(pTypeDef->ppClassTypeArgs[0]);
						PushPTR(pTypeDef);
					} else {
						PushOp(JIT_BOX_TYPEID + pStackType->stackType);
						PushPTR(pTypeDef);
					}
					// This is correct - cannot push underlying type, as then references are treated as value-types
					PushStackType(types[TYPE_SYSTEM_OBJECT]);
				}
				break;

			case CIL_UNBOX_ANY:
				{
					tMD_TypeDef *pTypeDef;

					PopStackTypeDontCare(); // Don't care what it is
					u32Value = GetUnalignedU32(pCIL, &cilOfs);
					pTypeDef = MetaData_GetTypeDefFromDefRefOrSpec(pMethodDef->pMetaData, u32Value, pMethodDef->pParentType->ppClassTypeArgs, pMethodDef->ppMethodTypeArgs);
					// isValueType is only known once the type is filled in; an enum nothing has touched
					// yet would otherwise look like a reference type and be routed to castclass
					MetaData_Fill_TypeDef(pTypeDef, NULL, NULL);
					if (pTypeDef->pGenericDefinition == types[TYPE_SYSTEM_NULLABLE]) {
						// This is a nullable type, so special unboxing is required.
						PushOp(JIT_UNBOX_NULLABLE);
						// For nullable types, push the underlying type, and then the nullable type itself
						PushPTR(pTypeDef->ppClassTypeArgs[0]);
						PushPTR(pTypeDef);
					} else if (pTypeDef->isValueType) {
						// The target type is an operand so the object's type can be checked against it
						MetaData_Fill_TypeDef(pTypeDef, NULL, NULL);
						PushOp(JIT_UNBOX2VALUETYPE);
						PushPTR(pTypeDef);
					} else {
						// unbox.any of a reference type is castclass (it used to be a no-op, so a
						// wrongly-typed object passed through unchecked)
						PushOp(JIT_CAST_CLASS);
						PushPTR(pTypeDef);
					}
					PushStackType(pTypeDef);
				}
				break;

			case CIL_LDTOKEN:
				u32Value = GetUnalignedU32(pCIL, &cilOfs);
				pMem = MetaData_GetTypeMethodField(pMethodDef->pMetaData, u32Value, &u32Value, pMethodDef->pParentType->ppClassTypeArgs, pMethodDef->ppMethodTypeArgs);
				PushOp(JIT_LOADTOKEN_BASE + u32Value);
				PushPTR(pMem);
				PushStackType(types[
					(u32Value==0)?TYPE_SYSTEM_RUNTIMETYPEHANDLE:
						((u32Value==1)?TYPE_SYSTEM_RUNTIMEFIELDHANDLE:TYPE_SYSTEM_RUNTIMEMETHODHANDLE)
				]);
				break;

			case CIL_THROW:
				PopStackTypeDontCare(); // Don't care what it is
				PushOp(JIT_THROW);
				RestoreTypeStack(&typeStack, ppTypeStacks[cilOfs]);
				break;

			case CIL_LEAVE_S:
				u32Value = (I8)pCIL[cilOfs++];
				goto cilLeave;

			case CIL_LEAVE:
				u32Value = GetUnalignedU32(pCIL, &cilOfs);
cilLeave:
				// Put a temporary CIL offset value into the JITted code. This will be updated later
				u32Value = cilOfs + (I32)u32Value;
				MayCopyTypeStack();
				RestoreTypeStack(&typeStack, ppTypeStacks[cilOfs]);
				PushOp(JIT_LEAVE);
				PushBranch();
				PushU32(u32Value);
				break;

			case CIL_ENDFINALLY:
				PushOp(JIT_END_FINALLY);
				RestoreTypeStack(&typeStack, ppTypeStacks[cilOfs]);
				break;

			case CIL_EXTENDED:
				op = pCIL[cilOfs++];

				switch (op)
				{
				case CILX_INITOBJ:
					{
						tMD_TypeDef *pTypeDef;

						PopStackTypeDontCare(); // Don't care what it is
						u32Value = GetUnalignedU32(pCIL, &cilOfs);
						pTypeDef = MetaData_GetTypeDefFromDefRefOrSpec(pMethodDef->pMetaData, u32Value, pMethodDef->pParentType->ppClassTypeArgs, pMethodDef->ppMethodTypeArgs);
						if (pTypeDef->isValueType) {
							PushOp(JIT_INIT_VALUETYPE);
							PushPTR(pTypeDef);
						} else {
							PushOp(JIT_INIT_OBJECT);
						}
					}
					break;

				case CILX_LOADFUNCTION:
					{
						tMD_MethodDef *pFuncMethodDef;

						u32Value = GetUnalignedU32(pCIL, &cilOfs);
						pFuncMethodDef = MetaData_GetMethodDefFromDefRefOrSpec(pMethodDef->pMetaData, u32Value, pMethodDef->pParentType->ppClassTypeArgs, pMethodDef->ppMethodTypeArgs);
						PushOp(JIT_LOADFUNCTION);
						PushPTR(pFuncMethodDef);
						PushStackType(types[TYPE_SYSTEM_INTPTR]);
					}
					break;

				case CILX_ENDFILTER:
					// Ends a filter; the int32 on the stack is its verdict. Control does not continue
					// into the next instruction (the guarded handler), so, as for ret, pick up that
					// instruction's recorded stack.
					PopStackTypeDontCare();
					PushOp(JIT_ENDFILTER);
					RestoreTypeStack(&typeStack, ppTypeStacks[cilOfs]);
					break;

				case CILX_CPBLK:
					PopStackTypeMulti(3); // destination, source, byte count
					PushOp(JIT_CPBLK);
					break;

				case CILX_INITBLK:
					PopStackTypeMulti(3); // address, value, byte count
					PushOp(JIT_INITBLK);
					break;

				case CILX_REFANYTYPE:
					PopStackTypeDontCare(); // the TypedReference
					PushOp(JIT_REFANYTYPE);
					PushStackType(types[TYPE_SYSTEM_RUNTIMETYPEHANDLE]);
					break;

				case CILX_LDVIRTFTN:
					{
						tMD_MethodDef *pVirtFuncMethodDef;

						PopStackTypeDontCare(); // the object
						u32Value = GetUnalignedU32(pCIL, &cilOfs);
						pVirtFuncMethodDef = MetaData_GetMethodDefFromDefRefOrSpec(pMethodDef->pMetaData, u32Value, pMethodDef->pParentType->ppClassTypeArgs, pMethodDef->ppMethodTypeArgs);
						if (pVirtFuncMethodDef->isFilled == 0) {
							// as for a call: the declaring type must be filled in before its vtable index is read
							tMD_TypeDef *pDeclaringType = MetaData_GetTypeDefFromMethodDef(pVirtFuncMethodDef);
							MetaData_Fill_TypeDef(pDeclaringType, NULL, NULL);
						}
						PushOp(JIT_LOADVIRTFUNCTION);
						PushPTR(pVirtFuncMethodDef);
						PushStackType(types[TYPE_SYSTEM_INTPTR]);
					}
					break;

				case CILX_CEQ:
				case CILX_CGT:
				case CILX_CGT_UN:
				case CILX_CLT:
				case CILX_CLT_UN:
					pTypeB = PopStackType();
					pTypeA = PopStackType();
					if (sizeof(void*) > 4 &&
						((pTypeA->stackType == EVALSTACK_O && pTypeB->stackType == EVALSTACK_O) ||
						 (pTypeA->stackType == EVALSTACK_PTR && pTypeB->stackType == EVALSTACK_PTR))) {
						// references and pointers are 8 bytes here, so compare them as 64-bit values
						PushOp(JIT_CEQ_I64I64 + (op - CILX_CEQ));
					} else if ((pTypeA->stackType == EVALSTACK_INT32 && pTypeB->stackType == EVALSTACK_INT32) ||
						(pTypeA->stackType == EVALSTACK_O && pTypeB->stackType == EVALSTACK_O) ||
						(pTypeA->stackType == EVALSTACK_PTR && pTypeB->stackType == EVALSTACK_PTR)) {
						PushOp(JIT_CEQ_I32I32 + (op - CILX_CEQ));
					} else if (pTypeA->stackType == EVALSTACK_INT64 && pTypeB->stackType == EVALSTACK_INT64) {
						PushOp(JIT_CEQ_I64I64 + (op - CILX_CEQ));
					} else if (pTypeA->stackType == EVALSTACK_F32 && pTypeB->stackType == EVALSTACK_F32) {
						PushOp(JIT_CEQ_F32F32 + (op - CILX_CEQ));
					} else if (pTypeA->stackType == EVALSTACK_F64 && pTypeB->stackType == EVALSTACK_F64) {
						PushOp(JIT_CEQ_F64F64 + (op - CILX_CEQ));
					} else {
						Crash("JITit(): Cannot perform comparison operand on stack types: %s and %s", pTypeA->name, pTypeB->name);
					}
					PushStackType(types[TYPE_SYSTEM_INT32]);
					break;
					
				case CILX_RETHROW:
					PushOp(JIT_RETHROW);
					break;

				case CILX_CONSTRAINED:
					u32Value2 = GetUnalignedU32(pCIL, &cilOfs);
					cilOfs++;
					goto cilCallVirtConstrained;

				case CILX_READONLY:
					// Do nothing
					break;

				case CILX_VOLATILE:
				case CILX_TAIL:
					// Prefixes with no effect here: the interpreter does no reordering or caching that
					// volatile. would have to defeat, and tail. is only a request to reuse the frame.
					break;

				case CILX_UNALIGNED:
					cilOfs++; // the alignment operand; accesses are never assumed aligned anyway
					break;

				// The two-byte forms take a 16-bit index (for methods with more than 255 args/locals)
				case CILX_LDARG:
					u32Value = pCIL[cilOfs] | ((U32)pCIL[cilOfs + 1] << 8); cilOfs += 2;
					goto cilLdArg;
				case CILX_LDARGA:
					u32Value = pCIL[cilOfs] | ((U32)pCIL[cilOfs + 1] << 8); cilOfs += 2;
					goto cilLdArgA;
				case CILX_STARG:
					u32Value = pCIL[cilOfs] | ((U32)pCIL[cilOfs + 1] << 8); cilOfs += 2;
					goto cilStArg;
				case CILX_LDLOC:
					u32Value = pCIL[cilOfs] | ((U32)pCIL[cilOfs + 1] << 8); cilOfs += 2;
					goto cilLdLoc;
				case CILX_LDLOCA:
					u32Value = pCIL[cilOfs] | ((U32)pCIL[cilOfs + 1] << 8); cilOfs += 2;
					goto cilLdLocA;
				case CILX_STLOC:
					u32Value = pCIL[cilOfs] | ((U32)pCIL[cilOfs + 1] << 8); cilOfs += 2;
					goto cilStLoc;

				case CILX_SIZEOF:
					{
						tMD_TypeDef *pSizeType;
						I32 size;

						u32Value = GetUnalignedU32(pCIL, &cilOfs);
						pSizeType = MetaData_GetTypeDefFromDefRefOrSpec(pMethodDef->pMetaData, u32Value, pMethodDef->pParentType->ppClassTypeArgs, pMethodDef->ppMethodTypeArgs);
						MetaData_Fill_TypeDef(pSizeType, NULL, NULL);
						if (!pSizeType->isValueType) {
							size = sizeof(void*);
						} else {
							// Unmanaged types have the reference runtime's layout; anything else
							// (a struct holding references) falls back to this runtime's own size.
							size = Marshal_GetUnmanagedSize(pSizeType);
							if (size < 0) {
								size = pSizeType->instanceMemSize;
							}
						}
						i32Value = size;
					}
					goto cilLdcI4;

				default:
					Crash("JITit(): JITter cannot handle extended op-code:0x%02x", op);

				}
				break;

			default:
				Crash("JITit(): JITter cannot handle op-code: 0x%02x", op);
		}

	} while (cilOfs < codeSize);

	// Replace runs of instructions that have a fused form (before the branch targets are converted, while every target
	// is still known from branchOffsets)
	FuseOps(&ops, &branchOffsets, pJITOffsets, instrList, numInstr, pJITted, codeSize, pMethodDef);

	// Apply branch offset fixes
	for (i=0; i<branchOffsets.ofs; i++) {
		U32 ofs, jumpTarget;

		ofs = branchOffsets.p[i];
		jumpTarget = ops.p[ofs];
		// Rewrite the branch offset
		jumpTarget = pJITOffsets[jumpTarget];
		ops.p[ofs] = jumpTarget;
#ifdef GEN_COMBINED_OPCODES
		isDynamic.p[jumpTarget] |= DYNAMIC_JUMP_TARGET;
#endif
	}

	// Apply expection handler offset fixes
	for (i=0; i<pJITted->numExceptionHandlers; i++) {
		tExceptionHeader *pEx;

		pEx = &pJITted->pExceptionHeaders[i];
		if (pEx->flags == COR_ILEXCEPTION_CLAUSE_FILTER) {
			pEx->u.filterOffset = pJITOffsets[pEx->u.filterOffset];
		}
		pEx->tryEnd = pJITOffsets[pEx->tryStart + pEx->tryEnd];
		pEx->tryStart = pJITOffsets[pEx->tryStart];
		pEx->handlerEnd = pJITOffsets[pEx->handlerStart + pEx->handlerEnd];
		pEx->handlerStart = pJITOffsets[pEx->handlerStart];
#ifdef GEN_COMBINED_OPCODES
		isDynamic.p[pEx->tryStart] |= DYNAMIC_EX_START | DYNAMIC_JUMP_TARGET;
		isDynamic.p[pEx->tryEnd] |= DYNAMIC_EX_END | DYNAMIC_JUMP_TARGET;
		isDynamic.p[pEx->handlerStart] |= DYNAMIC_EX_START | DYNAMIC_JUMP_TARGET;
		isDynamic.p[pEx->handlerEnd] |= DYNAMIC_EX_END | DYNAMIC_JUMP_TARGET;
#endif
	}

#ifdef GEN_COMBINED_OPCODES
	// Find any candidates for instruction combining
	// WARNING: The logic here doesn't yet corresponding fix up the sequence points we're tracking for debugging
	//          That's OK right now because none of this optimisation is actually used in current builds.
	if (genCombinedOpcodes) {
		U32 inst0 = 0;
		while (inst0 < ops.ofs) {
			U32 opCodeCount = 0;
			U32 instCount = 0;
			U32 shrinkOpsBy;
			U32 isFirstInst;
			while (!(isDynamic.p[inst0] & DYNAMIC_OK)) {
				inst0++;
				if (inst0 >= ops.ofs) {
					goto combineDone;
				}
			}
			isFirstInst = 1;
			while (isDynamic.p[inst0 + instCount] & DYNAMIC_OK) {
				if (isFirstInst) {
					isFirstInst = 0;
				} else {
					if (isDynamic.p[inst0 + instCount] & DYNAMIC_JUMP_TARGET) {
						// Cannot span a jump target
						break;
					}
				}
				instCount += 1 + ((isDynamic.p[inst0 + instCount] & DYNAMIC_BYTE_COUNT_MASK) >> 2);
				opCodeCount++;
			}
			shrinkOpsBy = 0;
			if (opCodeCount > 1) {
				U32 combinedSize;
				tCombinedOpcodesMem *pCOMem = TMALLOC(tCombinedOpcodesMem);
				shrinkOpsBy = GenCombined(&ops, &isDynamic, inst0, instCount, &combinedSize, &pCOMem->pMem);
				pCOMem->pNext = pJITted->pCombinedOpcodesMem;
				pJITted->pCombinedOpcodesMem = pCOMem;
				pJITted->opsMemSize += combinedSize;
				memmove(&ops.p[inst0 + instCount - shrinkOpsBy], &ops.p[inst0 + instCount], (ops.ofs - inst0 - instCount) << 2);
				memmove(&isDynamic.p[inst0 + instCount - shrinkOpsBy], &isDynamic.p[inst0 + instCount], (ops.ofs - inst0 - instCount) << 2);
				ops.ofs -= shrinkOpsBy;
				isDynamic.ofs -= shrinkOpsBy;
				for (i=0; i<branchOffsets.ofs; i++) {
					U32 ofs;
					if (branchOffsets.p[i] > inst0) {
						branchOffsets.p[i] -= shrinkOpsBy;
					}
					ofs = branchOffsets.p[i];
					if (ops.p[ofs] > inst0) {
						ops.p[ofs] -= shrinkOpsBy;
					}
				}
				for (i=0; i<pJITted->numExceptionHandlers; i++) {
					tExceptionHeader *pEx;

					pEx = &pJITted->pExceptionHeaders[i];
					if (pEx->tryStart > inst0) {
						pEx->tryStart -= shrinkOpsBy;
					}
					if (pEx->tryEnd > inst0) {
						pEx->tryEnd -= shrinkOpsBy;
					}
					if (pEx->handlerStart > inst0) {
						pEx->handlerStart -= shrinkOpsBy;
					}
					if (pEx->handlerEnd > inst0) {
						pEx->handlerEnd -= shrinkOpsBy;
					}
				}
			}
			inst0 += instCount - shrinkOpsBy;
		}
	}
combineDone:
#endif

	// Change maxStack to indicate the number of bytes needed on the evaluation stack.
	// This is the largest number of bytes needed by all objects/value-types on the stack,
	pJITted->maxStack = typeStack.maxBytes + pJITted->inlineExtraStack;   // (what the methods inlined into native blocks need)

	free(typeStack.ppTypes);

	for (i=0; i<codeSize; i++) {
		if (ppTypeStacks[i] != NULL) {
			free(ppTypeStacks[i]->ppTypes);
		}
	}
	free(ppTypeStacks);

	DeleteOps(branchOffsets);
	if (ppCilToOp != NULL) {                                   // (the op that each CIL instruction starts at; meaningful for the first byte of an instruction)
		*ppCilToOp = (U32*)malloc(codeSize * sizeof(U32));
		memcpy(*ppCilToOp, pJITOffsets, codeSize * sizeof(U32));
	}
	free(pJITOffsets);
	free(instrList);

	// Copy ops to some memory of exactly the correct size. To not waste memory.
	// (The op stream is made of tOpWord, and the sequence points of I32: they are different sizes on 64-bit.)
	u32Value = ops.ofs * sizeof(tOpWord);
	pFinalOps = genCombinedOpcodes?malloc(u32Value):mallocForever(u32Value);
	memcpy(pFinalOps, ops.p, u32Value);
	
	pJITted->pDebugMetadataEntry = pDebugMetadataEntry;
	if (pDebugMetadataEntry != NULL) {
		U32 seqBytes = ops.ofs * sizeof(I32);
		*ppSequencePoints = mallocForever(seqBytes);
		memcpy(*ppSequencePoints, ops.pSequencePoints, seqBytes);
	} else {
		// This method has no debug info
		*ppSequencePoints = NULL;
	}

	DeleteOps(ops);
#ifdef GEN_COMBINED_OPCODES
	pJITted->opsMemSize += u32Value;
	DeleteOps(isDynamic);
#endif

	return pFinalOps;
}

// Prepare a method for execution
// This makes sure that the method has been JITed.
void JIT_Prepare(tMD_MethodDef *pMethodDef, U32 genCombinedOpcodes) {
	tMetaData *pMetaData;
	U8 *pMethodHeader;
	tJITted *pJITted;
	FLAGS16 flags;
	U32 codeSize;
	IDX_TABLE localsToken;
	U8 *pCIL;
	SIG sig;
	U32 i, sigLength, numLocals;
	tParameter *pLocals;

	log_f(2, "JIT:   %s\n", Sys_GetMethodDesc(pMethodDef));

	pMetaData = pMethodDef->pMetaData;
	pJITted = (genCombinedOpcodes)?TMALLOC(tJITted):TMALLOCFOREVER(tJITted);
	// (malloc does not clear it; pOps in particular must be NULL until the method has been compiled, which is how a method that is
	// still being compiled is told from one that is done)
	memset(pJITted, 0, sizeof(tJITted));
#ifdef GEN_COMBINED_OPCODES
	pJITted->pCombinedOpcodesMem = NULL;
	pJITted->opsMemSize = 0;
	if (genCombinedOpcodes) {
		pMethodDef->pJITtedCombined = pJITted;
	} else {
		pMethodDef->pJITted = pJITted;
	}
#else
	pMethodDef->pJITted = pJITted;
#endif

	if ((pMethodDef->implFlags & METHODIMPLATTRIBUTES_INTERNALCALL) ||
		((pMethodDef->implFlags & METHODIMPLATTRIBUTES_CODETYPE_MASK) == METHODIMPLATTRIBUTES_CODETYPE_RUNTIME)) {
		tJITCallNative *pCallNative;

		// Internal call
		if (strcmp(pMethodDef->name, ".ctor") == 0) {
			// Internal constructor needs enough evaluation stack space to return itself
			pJITted->maxStack = pMethodDef->pParentType->stackSize;
		} else {
			pJITted->maxStack = (pMethodDef->pReturnType == NULL)?0:pMethodDef->pReturnType->stackSize; // For return value
		}
		pCallNative = TMALLOCFOREVER(tJITCallNative);
		pCallNative->opCode = Translate(JIT_CALL_NATIVE, 0);
		pCallNative->pMethodDef = pMethodDef;
		pCallNative->fn = InternalCall_Map(pMethodDef);
		pCallNative->retOpCode = Translate(JIT_RETURN, 0);

		pJITted->localsStackSize = 0;
		pJITted->pOps = (tOpWord*)pCallNative;
		pJITted->pOpSequencePoints = NULL;

		return;
	}
	if (pMethodDef->flags & METHODATTRIBUTES_PINVOKEIMPL) {
		tJITCallPInvoke *pCallPInvoke;

		// PInvoke call
		tMD_ImplMap *pImplMap = MetaData_GetImplMap(pMetaData, pMethodDef->tableIndex);
		const tFFIEntry *pFfi = FFI_Find(MetaData_GetModuleRefName(pMetaData, pImplMap->importScope), pImplMap->importName);
		fnPInvoke fn = NULL;
		if (pFfi != NULL) {
			// named in the FFI manifest: called through its wrapper (from here, with the parameters in a frame)
			const char *pBad = FFI_CheckSignature(pFfi, pMethodDef);
			if (pBad != NULL) { Crash("FFI manifest and DllImport disagree: %s", pBad); }
		} else {
			fn = PInvoke_GetFunction(pMetaData, pImplMap);
			if (fn == NULL) {
				Crash("PInvoke library or function not found: %s()", pImplMap->importName);
			}
		}

		pCallPInvoke = TMALLOCFOREVER(tJITCallPInvoke);
		pCallPInvoke->opCode = Translate(JIT_CALL_PINVOKE, 0);
		pCallPInvoke->fn = fn;
		pCallPInvoke->ffi = pFfi;
		pCallPInvoke->pMethod = pMethodDef;
		pCallPInvoke->pImplMap = pImplMap;

		pJITted->localsStackSize = 0;
		pJITted->maxStack = (pMethodDef->pReturnType == NULL)?0:pMethodDef->pReturnType->stackSize; // For return value
		pJITted->pOps = (tOpWord*)pCallPInvoke;
		pJITted->pOpSequencePoints = NULL;

		return;
	}

	pMethodHeader = (U8*)pMethodDef->pCIL;
	if ((*pMethodHeader & 0x3) == CorILMethod_TinyFormat) {
		// Tiny header
		flags = *pMethodHeader & 0x3;
		pJITted->maxStack = 8;
		codeSize = (*pMethodHeader & 0xfc) >> 2;
		localsToken = 0;
		pCIL = pMethodHeader + 1;
	} else {
		// Fat header
		flags = *(U16*)pMethodHeader & 0x0fff;
		pJITted->maxStack = *(U16*)&pMethodHeader[2];
		codeSize = *(U32*)&pMethodHeader[4];
		localsToken = *(IDX_TABLE*)&pMethodHeader[8];
		pCIL = pMethodHeader + ((pMethodHeader[1] & 0xf0) >> 2);
	}
	if (flags & CorILMethod_MoreSects) {
		U32 numClauses;

		pMethodHeader = pCIL + ((codeSize + 3) & (~0x3));
		if (*pMethodHeader & CorILMethod_Sect_FatFormat) {
			U32 exSize;
			// Fat header
			numClauses = ((*(U32*)pMethodHeader >> 8) - 4) / 24;
			//pJITted->pExceptionHeaders = (tExceptionHeader*)(pMethodHeader + 4);
			exSize = numClauses * sizeof(tExceptionHeader);
			pJITted->pExceptionHeaders =
				(tExceptionHeader*)(genCombinedOpcodes?malloc(exSize):mallocForever(exSize));
			memset(pJITted->pExceptionHeaders, 0, exSize);
			// The file holds 24 bytes per clause. They were copied straight over the structs, which only
			// worked while tExceptionHeader happened to be exactly 24 bytes (it is 32 on a 64-bit target,
			// where the union holds a pointer), so each clause is read field by field.
			for (i=0; i<numClauses; i++) {
				U32 w[6];
				memcpy(w, pMethodHeader + 4 + i * 24, sizeof(w));
				pJITted->pExceptionHeaders[i].flags = w[0];
				pJITted->pExceptionHeaders[i].tryStart = w[1];
				pJITted->pExceptionHeaders[i].tryEnd = w[2];        // (a length, until the offsets are fixed up)
				pJITted->pExceptionHeaders[i].handlerStart = w[3];
				pJITted->pExceptionHeaders[i].handlerEnd = w[4];    // (likewise)
				pJITted->pExceptionHeaders[i].u.classToken = w[5];  // or the filter's offset
			}
		} else {
			// Thin header
			tExceptionHeader *pExHeaders;
			U32 exSize;

			numClauses = (((U8*)pMethodHeader)[1] - 4) / 12;
			exSize = numClauses * sizeof(tExceptionHeader);
			pMethodHeader += 4;
			//pExHeaders = pJITted->pExceptionHeaders = (tExceptionHeader*)mallocForever(numClauses * sizeof(tExceptionHeader));
			pExHeaders = pJITted->pExceptionHeaders =
				(tExceptionHeader*)(genCombinedOpcodes?malloc(exSize):mallocForever(exSize));
			memset(pExHeaders, 0, exSize);
			for (i=0; i<numClauses; i++) {
				pExHeaders[i].flags = ((U16*)pMethodHeader)[0];
				pExHeaders[i].tryStart = ((U16*)pMethodHeader)[1];
				pExHeaders[i].tryEnd = ((U8*)pMethodHeader)[4];
				pExHeaders[i].handlerStart = ((U8*)pMethodHeader)[5] | (((U8*)pMethodHeader)[6] << 8);
				pExHeaders[i].handlerEnd = ((U8*)pMethodHeader)[7];
				pExHeaders[i].u.classToken = ((U32*)pMethodHeader)[2];

				pMethodHeader += 12;
			}
		}
		pJITted->numExceptionHandlers = numClauses;
		// replace all classToken's with the actual tMD_TypeDef*
		for (i=0; i<numClauses; i++) {
			if (pJITted->pExceptionHeaders[i].flags == COR_ILEXCEPTION_CLAUSE_EXCEPTION) {
				pJITted->pExceptionHeaders[i].u.pCatchTypeDef =
					MetaData_GetTypeDefFromDefRefOrSpec(pMethodDef->pMetaData, pJITted->pExceptionHeaders[i].u.classToken, pMethodDef->pParentType->ppClassTypeArgs, pMethodDef->ppMethodTypeArgs);
			}
		}
	} else {
		pJITted->numExceptionHandlers = 0;
		pJITted->pExceptionHeaders = NULL;
	}

	// Analyse the locals
	if (localsToken == 0) {
		// No locals
		pJITted->localsStackSize = 0;
		pLocals = NULL;
	} else {
		tMD_StandAloneSig *pStandAloneSig;
		U32 i, totalSize;

		pStandAloneSig = (tMD_StandAloneSig*)MetaData_GetTableRow(pMethodDef->pMetaData, localsToken);
		sig = MetaData_GetBlob(pStandAloneSig->signature, &sigLength);
		MetaData_DecodeSigEntry(&sig); // Always 0x07
		numLocals = MetaData_DecodeSigEntry(&sig);
		pLocals = (tParameter*)malloc(numLocals * sizeof(tParameter));
		totalSize = 0;
		for (i=0; i<numLocals; i++) {
			tMD_TypeDef *pTypeDef;

			pTypeDef = Type_GetTypeFromSig(pMethodDef->pMetaData, &sig, pMethodDef->pParentType->ppClassTypeArgs, pMethodDef->ppMethodTypeArgs);
			MetaData_Fill_TypeDef(pTypeDef, NULL, NULL);
			pLocals[i].pTypeDef = pTypeDef;
			pLocals[i].offset = totalSize;
			pLocals[i].size = pTypeDef->stackSize;
			totalSize += pTypeDef->stackSize;
		}
		pJITted->localsStackSize = totalSize;
	}

#if WASM_JIT
	// A method in the subset that WasmJIT.c can compile becomes a WebAssembly function: its instruction stream is only the call of it.
	// (Not one with exception clauses; those stay with the interpreter.)
	if (!(flags & CorILMethod_MoreSects)) {
		tWasmResult wasmRes; U32 wasmFn = WasmJIT_Compile(pMethodDef, pCIL, codeSize, pLocals, localsToken == 0 ? 0 : numLocals, pJITted->localsStackSize, &wasmRes);
		if (wasmFn != 0) {
			U32 retSize = pMethodDef->pReturnType == NULL ? 0 : pMethodDef->pReturnType->stackSize;
			tOpWord *stub = (tOpWord*)mallocForever(5 * sizeof(tOpWord));
			stub[0] = Translate(JIT_WASM_METHOD, 0);
			stub[1] = (tOpWord)wasmFn;
			stub[2] = (tOpWord)retSize;
			stub[3] = (tOpWord)(uintptr_t)wasmRes.deopt;
			pJITted->pWasmDeopt = wasmRes.deopt;
			stub[4] = Translate(JIT_RETURN, 0);
			pJITted->maxStack = wasmRes.maxStack > retSize ? wasmRes.maxStack : retSize;       // (the interpreter's version, after a deoptimization, uses the same evaluation stack)
			pJITted->localsStackSize += wasmRes.extraFrame + (wasmRes.deopt != NULL ? WJ_DEOPT_SLACK : 0);       // (the scratch area for struct values and for the functions it calls)
			pJITted->pOps = stub;
			pJITted->pOpSequencePoints = NULL;
			free(pLocals);
			return;
		}
	}
#endif

	// JIT the CIL code
	I32 *pSequencePoints;
	pJITted->pOps = JITit(pMethodDef, pCIL, codeSize, pLocals, pJITted, genCombinedOpcodes, &pSequencePoints, NULL);
	pJITted->pOpSequencePoints = pSequencePoints;

	free(pLocals);
}

#if WASM_JIT
// The interpreter's version of a method, for a frame that was running compiled code and has to go on in the interpreter (WasmJIT.h: deoptimization).
// It has the same frame (parameters, then locals) as the compiled version, and its own evaluation stack size: the compiled version's must be at
// least that. *ppCilToOp is the op at which each CIL instruction starts.
void JIT_BuildInterpreterVersion(tMD_MethodDef *pMethodDef, U8 *pCIL, U32 codeSize, tParameter *pLocals, U32 headerMaxStack, U32 localsStackSize,
		const U32 *keep, U32 numKeep, const U32 *osr, U32 numOsr, tJITted **ppOut, U32 **ppCilToOp) {
	tJITted *pJ = (tJITted*)mallocForever(sizeof(tJITted));
	I32 *pSeq = NULL;
	memset(pJ, 0, sizeof(tJITted));
	pJ->maxStack = headerMaxStack;
	pJ->localsStackSize = localsStackSize;
	jitKeep = keep; jitNumKeep = numKeep; jitOsr = osr; jitNumOsr = numOsr;
	pJ->pOps = JITit(pMethodDef, pCIL, codeSize, pLocals, pJ, 0, &pSeq, ppCilToOp);
	jitKeep = NULL; jitNumKeep = 0; jitOsr = NULL; jitNumOsr = 0;
	pJ->pOpSequencePoints = pSeq;
	*ppOut = pJ;
}

// A receiver whose type was not expected failed the guards of a call in this method (WasmJIT_NoteMiss): compile the method again, with that type among the targets,
// and make that its compiled version. Frames of the version before it can still be live (one that gave up the processor, or that is going on in the interpreter), and
// they are sized from the method's current version: so the new one must fit them and takes their sizes, or else the old one stays.
int JIT_RecompileWasm(tMD_MethodDef *pMethodDef) {
	tJITted *pOld = pMethodDef->pJITted, *pNew;
	tWasmDeopt *pOldDeopt = pOld == NULL ? NULL : (tWasmDeopt*)pOld->pWasmDeopt;
	tWasmResult res;
	U32 fn, retSize;
	tOpWord *stub;
	if (pOldDeopt == NULL || pOldDeopt->recompiles >= 4) { return 0; }
	fn = WasmJIT_Compile(pMethodDef, pOldDeopt->cil, pOldDeopt->codeSize, pOldDeopt->locals, pOldDeopt->numLocals, pOldDeopt->origLocalsSize, &res);
	if (fn == 0) { return 0; }
	retSize = pMethodDef->pReturnType == NULL ? 0 : pMethodDef->pReturnType->stackSize;
	if (res.deopt == NULL || (res.maxStack > retSize ? res.maxStack : retSize) > pOld->maxStack || pOldDeopt->origLocalsSize + res.extraFrame > pOld->localsStackSize) { return 0; }
	pNew = (tJITted*)mallocForever(sizeof(tJITted));
	*pNew = *pOld;
	stub = (tOpWord*)mallocForever(5 * sizeof(tOpWord));
	stub[0] = Translate(JIT_WASM_METHOD, 0);
	stub[1] = (tOpWord)fn;
	stub[2] = (tOpWord)retSize;
	stub[3] = (tOpWord)(uintptr_t)res.deopt;
	stub[4] = Translate(JIT_RETURN, 0);
	pNew->pOps = stub;
	pNew->pWasmDeopt = res.deopt;
	res.deopt->recompiles = pOldDeopt->recompiles + 1;
	pMethodDef->pJITted = pNew;
	return 1;
}
#endif
