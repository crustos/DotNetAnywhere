// Native half of System.Runtime.InteropServices.Marshal / MemoryMarshal.
// See the header for what layout this implements and why it isn't a memcpy.

#include "Compat.h"
#include "Sys.h"

#include "System.Runtime.InteropServices.Marshal.h"

#include "Types.h"
#include "MetaData.h"
#include "MetaDataTables.h"
#include "Heap.h"
#include "Type.h"
#include "System.Array.h"
#include "System.RuntimeType.h"

#define MD_TABLE_CLASSLAYOUT 0x0f
#define MAX_DEPTH 32

enum { MODE_LAYOUT = 0, MODE_TO_NET = 1, MODE_FROM_NET = 2 };

static int HostIsLittleEndian(void) {
	U32 one = 1;
	return *(U8*)&one == 1;
}

// Size / alignment / signedness of a primitive; returns 0 if t is not one.
static int Primitive(tMD_TypeDef *t, U32 *pSize, int *pSigned) {
	*pSigned = 0;
	if (t == types[TYPE_SYSTEM_BOOLEAN] || t == types[TYPE_SYSTEM_BYTE]) { *pSize = 1; return 1; }
	if (t == types[TYPE_SYSTEM_SBYTE]) { *pSize = 1; *pSigned = 1; return 1; }
	if (t == types[TYPE_SYSTEM_CHAR] || t == types[TYPE_SYSTEM_UINT16]) { *pSize = 2; return 1; }
	if (t == types[TYPE_SYSTEM_INT16]) { *pSize = 2; *pSigned = 1; return 1; }
	if (t == types[TYPE_SYSTEM_UINT32] || t == types[TYPE_SYSTEM_SINGLE]) { *pSize = 4; return 1; }
	if (t == types[TYPE_SYSTEM_INT32]) { *pSize = 4; *pSigned = 1; return 1; }
	if (t == types[TYPE_SYSTEM_UINT64] || t == types[TYPE_SYSTEM_DOUBLE]) { *pSize = 8; return 1; }
	if (t == types[TYPE_SYSTEM_INT64]) { *pSize = 8; *pSigned = 1; return 1; }
	if (t == types[TYPE_SYSTEM_INTPTR]) { *pSize = sizeof(void*); *pSigned = 1; return 1; }
	if (t == types[TYPE_SYSTEM_UINTPTR]) { *pSize = sizeof(void*); return 1; }
	return 0;
}

// [StructLayout(Pack = n)] -> n, or 0 for the default (natural alignment).
static U32 GetPack(tMD_TypeDef *t) {
	tMetaData *pMetaData = t->pMetaData;
	IDX_TABLE self = (t->pGenericDefinition != NULL) ? t->pGenericDefinition->tableIndex : t->tableIndex;
	U32 i, n = pMetaData->tables.numRows[MD_TABLE_CLASSLAYOUT];
	for (i = 1; i <= n; i++) {
		tMD_ClassLayout *pRow = (tMD_ClassLayout*)MetaData_GetTableRow(pMetaData, MAKE_TABLE_INDEX(MD_TABLE_CLASSLAYOUT, i));
		if (pRow->parent == self) {
			return pRow->packingSize;
		}
	}
	return 0;
}

static U32 AlignUp(U32 v, U32 a) { return (v + a - 1) / a * a; }

// Layout (MODE_LAYOUT) or copy one value between DNA memory and .NET bytes.
//   dna: the value's DNA instance memory      (unused for MODE_LAYOUT)
//   net: where its .NET-layout bytes are/go   (unused for MODE_LAYOUT)
// Always computes size/align. Returns 0, or 1 if t is not unmanaged.
static int Walk(tMD_TypeDef *t, U8 *dna, U8 *net, int mode, int depth, U32 *pSize, U32 *pAlign) {
	U32 psize;
	int psigned;

	if (depth > MAX_DEPTH || t == NULL) {
		return 1;
	}

	if (Primitive(t, &psize, &psigned)) {
		*pSize = psize;
		*pAlign = psize;
		if (mode == MODE_TO_NET) {
			memcpy(net, dna, psize);
		} else if (mode == MODE_FROM_NET) {
			U32 slot = t->stackSize;
			memset(dna, 0, slot);
			memcpy(dna, net, psize);
			// The evaluation stack holds small integers widened to 32 bits.
			if (psigned && psize < slot && (net[psize - 1] & 0x80)) {
				memset(dna + psize, 0xff, slot - psize);
			}
		}
		return 0;
	}

	if (t->pParent == types[TYPE_SYSTEM_ENUM]) {
		// An enum's one non-literal field is its underlying integer.
		U32 i;
		for (i = 0; i < t->numFields; i++) {
			tMD_FieldDef *pField = t->ppFields[i];
			if (!FIELD_ISSTATIC(pField)) {
				return Walk(pField->pType, dna, net, mode, depth + 1, pSize, pAlign);
			}
		}
		return 1;
	}

	if (t->isValueType && !TYPE_ISARRAY(t) && !TYPE_ISINTERFACE(t)) {
		U32 pack = GetPack(t);
		U32 off = 0, maxAlign = 1, i;

		for (i = 0; i < t->numFields; i++) {
			tMD_FieldDef *pField = t->ppFields[i];
			U32 fsize, falign;

			if (FIELD_ISSTATIC(pField)) {
				continue;
			}
			if (Walk(pField->pType, NULL, NULL, MODE_LAYOUT, depth + 1, &fsize, &falign)) {
				return 1;
			}
			if (pack != 0 && pack < falign) {
				falign = pack;
			}
			off = AlignUp(off, falign);
			if (mode != MODE_LAYOUT) {
				U32 s2, a2;
				if (Walk(pField->pType, dna + pField->memOffset, net + off, mode, depth + 1, &s2, &a2)) {
					return 1;
				}
			}
			off += fsize;
			if (falign > maxAlign) {
				maxAlign = falign;
			}
		}
		if (off == 0) {
			off = 1; // an empty struct still occupies a byte
		}
		*pAlign = maxAlign;
		*pSize = AlignUp(off, maxAlign);
		return 0;
	}

	return 1; // reference type, string, array, ...
}

I32 Marshal_GetUnmanagedSize(tMD_TypeDef *pType) {
	U32 size, align;
	return Walk(pType, NULL, NULL, MODE_LAYOUT, 0, &size, &align) ? -1 : (I32)size;
}

tAsyncCall* System_Runtime_InteropServices_Marshal_SizeOfImpl(PTR pThis_, PTR pParams, PTR pReturnValue) {
	tMD_TypeDef *pType = RuntimeType_DeRef((PTR)((tMD_TypeDef**)pParams)[0]);
	U32 size, align;

	*(I32*)pReturnValue = Walk(pType, NULL, NULL, MODE_LAYOUT, 0, &size, &align) ? -1 : (I32)size;
	return NULL;
}

tAsyncCall* System_Runtime_InteropServices_MemoryMarshal_Serialize(PTR pThis_, PTR pParams, PTR pReturnValue) {
	tMD_TypeDef *pType = RuntimeType_DeRef((PTR)((tMD_TypeDef**)pParams)[0]);
	HEAP_PTR boxed = ((HEAP_PTR*)pParams)[1];
	HEAP_PTR dst = ((HEAP_PTR*)pParams)[2];
	I32 offset = INTERNALCALL_PARAM(3*PSZ, I32);
	U32 size, align;

	if (!HostIsLittleEndian()) {
		*(I32*)pReturnValue = 3;
	} else if (Walk(pType, NULL, NULL, MODE_LAYOUT, 0, &size, &align)) {
		*(I32*)pReturnValue = 1;
	} else if (offset < 0 || (U64)offset + size > SystemArray_GetLength(dst)) {
		*(I32*)pReturnValue = 2;
	} else {
		Walk(pType, boxed, SystemArray_GetElements(dst) + offset, MODE_TO_NET, 0, &size, &align);
		*(I32*)pReturnValue = 0;
	}
	return NULL;
}

tAsyncCall* System_Runtime_InteropServices_MemoryMarshal_Deserialize(PTR pThis_, PTR pParams, PTR pReturnValue) {
	tMD_TypeDef *pType = RuntimeType_DeRef((PTR)((tMD_TypeDef**)pParams)[0]);
	HEAP_PTR src = ((HEAP_PTR*)pParams)[1];
	I32 offset = INTERNALCALL_PARAM(2*PSZ, I32);
	U32 size, align;
	HEAP_PTR boxed;

	*(HEAP_PTR*)pReturnValue = NULL;
	if (!HostIsLittleEndian() || Walk(pType, NULL, NULL, MODE_LAYOUT, 0, &size, &align)) {
		return NULL;
	}
	if (offset < 0 || (U64)offset + size > SystemArray_GetLength(src)) {
		return NULL;
	}
	boxed = Heap_AllocType(pType);
	Walk(pType, boxed, SystemArray_GetElements(src) + offset, MODE_FROM_NET, 0, &size, &align);
	*(HEAP_PTR*)pReturnValue = boxed;
	return NULL;
}
