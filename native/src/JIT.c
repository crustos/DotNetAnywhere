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
#include "System.Runtime.InteropServices.Marshal.h"
#include "CIL_OpCodes.h"
#include "CLIFile.h"

#include "MetaData.h"
#include "Types.h"
#include "Type.h"
#include "InternalCall.h"
#include "Heap.h"
#include "PInvoke.h"

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

#define PushStackType(type) PushStackType_(&typeStack, type);
#define PopStackType() (typeStack.ppTypes[--typeStack.ofs])
#define PopStackTypeDontCare() typeStack.ofs--
#define PopStackTypeMulti(number) typeStack.ofs -= number
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

static SetBreakPoint(tMD_MethodDef *pMethodDef, U32 cilOfs, tOps ops)
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

static tOpWord* JITit(tMD_MethodDef *pMethodDef, U8 *pCIL, U32 codeSize, tParameter *pLocals, tJITted *pJITted, U32 genCombinedOpcodes, I32 **ppSequencePoints) {
	U32 maxStack = pJITted->maxStack;
	U32 i;
	U32 cilOfs;
	tOps ops; // The JITted op-codes
	tOps branchOffsets; // Filled with all the branch instructions that need offsets fixing
	U32 *pJITOffsets;	// To store the JITted code offset of each CIL byte.
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

			ppTypeStacks[pEx->handlerStart] = pTypeStack = TMALLOC(tTypeStack);
			pTypeStack->maxBytes = 4;
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
				pTypeStack->maxBytes = 4;
				pTypeStack->ofs = 1;
				pTypeStack->ppTypes = TMALLOC(tMD_TypeDef*);
				pTypeStack->ppTypes[0] = types[TYPE_SYSTEM_OBJECT];
			}
		}
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
					U32 derefRefType;
					U8 dynamicallyBoxReturnValue;

					u32Value2 = 0;

cilCallVirtConstrained:
					pBoxCallType = NULL;
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
					if (TYPE_ISINTERFACE(pCallMethod->pParentType) && op == CIL_CALLVIRT) {
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
					PushPTR(pCallMethod);

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
					PushOp(opCodeBase + convOpOffset);
					if (useParam) {
						PushU32(param);
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

			case CIL_LDELEM_I1:
			case CIL_LDELEM_U1:
			case CIL_LDELEM_I2:
			case CIL_LDELEM_U2:
			case CIL_LDELEM_I4:
			case CIL_LDELEM_U4:
				PopStackTypeMulti(2); // Don't care what any of these are
				PushOp(JIT_LOAD_ELEMENT_I8 + (op - CIL_LDELEM_I1));
				PushStackType(types[TYPE_SYSTEM_INT32]);
				break;

			case CIL_LDELEM_I8:
				PopStackTypeMulti(2); // Don't care what any of these are
				PushOp(JIT_LOAD_ELEMENT_I64);
				PushStackType(types[TYPE_SYSTEM_INT64]);
				break;

			case CIL_LDELEM_R4:
				PopStackTypeMulti(2); // Don't care what any of these are
				PushOp(JIT_LOAD_ELEMENT_R32);
				PushStackType(types[TYPE_SYSTEM_SINGLE]);
				break;

			case CIL_LDELEM_R8:
				PopStackTypeMulti(2); // Don't care what any of these are
				PushOp(JIT_LOAD_ELEMENT_R64);
				PushStackType(types[TYPE_SYSTEM_DOUBLE]);
				break;

			case CIL_LDELEM_REF:
				PopStackTypeMulti(2); // Don't care what any of these are
				PushOp(JIT_LOAD_ELEMENT_PTR);
				PushStackType(types[TYPE_SYSTEM_OBJECT]);
				break;

			case CIL_LDELEM_I: // native int: pointer-sized
				PopStackTypeMulti(2); // Don't care what any of these are
				PushOp(JIT_LOAD_ELEMENT_PTR);
				PushStackType(types[TYPE_SYSTEM_INTPTR]);
				break;

			case CIL_LDELEM_ANY:
				u32Value = GetUnalignedU32(pCIL, &cilOfs);
				pStackType = (tMD_TypeDef*)MetaData_GetTypeDefFromDefRefOrSpec(pMetaData, u32Value, pMethodDef->pParentType->ppClassTypeArgs, pMethodDef->ppMethodTypeArgs);
				PopStackTypeMulti(2); // Don't care what these are
				PushOpParam(JIT_LOAD_ELEMENT, pStackType->stackSize);
				PushStackType(pStackType);
				break;

			case CIL_LDELEMA:
				PopStackTypeMulti(2); // Don't care what any of these are
				GetUnalignedU32(pCIL, &cilOfs); // Don't care what this is
				PushOp(JIT_LOAD_ELEMENT_ADDR);
				PushStackType(types[TYPE_SYSTEM_INTPTR]);
				break;

			case CIL_STELEM_I1:
			case CIL_STELEM_I2:
			case CIL_STELEM_I4:
			case CIL_STELEM_R4:
				PopStackTypeMulti(3); // Don't care what any of these are
				PushOp(JIT_STORE_ELEMENT_32);
				break;

			case CIL_STELEM_I:   // native int and references are pointer-sized
			case CIL_STELEM_REF:
				PopStackTypeMulti(3); // Don't care what any of these are
				PushOp(JIT_STORE_ELEMENT_PTR);
				break;

			case CIL_STELEM_I8:
			case CIL_STELEM_R8:
				PopStackTypeMulti(3); // Don't care what any of these are
				PushOp(JIT_STORE_ELEMENT_64);
				break;

			case CIL_STELEM_ANY:
				GetUnalignedU32(pCIL, &cilOfs); // Don't need this token, as the type stack will contain the same type
				pStackType = PopStackType(); // This is the type to store
				PopStackTypeMulti(2); // Don't care what these are
				PushOpParam(JIT_STORE_ELEMENT, pStackType->stackSize);
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
						// Push the underlying type of the nullable type, not the nullable type itself
						PushPTR(pTypeDef->ppClassTypeArgs[0]);
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
						// For nullable types, push the underlying type
						PushPTR(pTypeDef->ppClassTypeArgs[0]);
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
	pJITted->maxStack = typeStack.maxBytes;

	free(typeStack.ppTypes);

	for (i=0; i<codeSize; i++) {
		if (ppTypeStacks[i] != NULL) {
			free(ppTypeStacks[i]->ppTypes);
		}
	}
	free(ppTypeStacks);

	DeleteOps(branchOffsets);
	free(pJITOffsets);

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
		fnPInvoke fn = PInvoke_GetFunction(pMetaData, pImplMap);
		if (fn == NULL) {
			Crash("PInvoke library or function not found: %s()", pImplMap->importName);
		}

		pCallPInvoke = TMALLOCFOREVER(tJITCallPInvoke);
		pCallPInvoke->opCode = Translate(JIT_CALL_PINVOKE, 0);
		pCallPInvoke->fn = fn;
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
			memcpy(pJITted->pExceptionHeaders, pMethodHeader + 4, exSize);
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

	// JIT the CIL code
	I32 *pSequencePoints;
	pJITted->pOps = JITit(pMethodDef, pCIL, codeSize, pLocals, pJITted, genCombinedOpcodes, &pSequencePoints);
	pJITted->pOpSequencePoints = pSequencePoints;

	free(pLocals);
}