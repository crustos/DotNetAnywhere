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

#include "System.Enum.h"

#include "MetaData.h"
#include "MetaDataTables.h"
#include "Types.h"
#include "Type.h"
#include "System.RuntimeType.h"
#include "System.Array.h"
#include "System.String.h"

tAsyncCall* System_Enum_Internal_GetValue(PTR pThis_, PTR pParams, PTR pReturnValue) {
	*(U32*)pReturnValue = *(U32*)pThis_;

	return NULL;
}

tAsyncCall* System_Enum_Internal_GetKind(PTR pThis_, PTR pParams, PTR pReturnValue) {
	tMD_TypeDef *pEnumType = RuntimeType_DeRef((PTR)((tMD_TypeDef**)pParams)[0]);
	tMetaData *pMetaData = pEnumType->pMetaData;
	U32 i, kind = 0;

	// The one non-literal field is the underlying integer
	for (i=0; i<pEnumType->numFields; i++) {
		tMD_FieldDef *pField = pEnumType->ppFields[i];
		if (!FIELD_ISSTATIC(pField)) {
			tMD_TypeDef *pUnder = pField->pType;
			if (pUnder == types[TYPE_SYSTEM_SBYTE] || pUnder == types[TYPE_SYSTEM_INT16] ||
				pUnder == types[TYPE_SYSTEM_INT32] || pUnder == types[TYPE_SYSTEM_INT64]) {
				kind |= 1;
			}
			kind |= ((U32)pUnder->arrayElementSize) << 8;
			break;
		}
	}

	// Is the enum marked [Flags]? Look for System.FlagsAttribute among its custom attributes.
	for (i=1; i<=pMetaData->tables.numRows[MD_TABLE_CUSTOMATTRIBUTE]; i++) {
		tMD_CustomAttribute *pAttr = (tMD_CustomAttribute*)MetaData_GetTableRow(pMetaData, MAKE_TABLE_INDEX(MD_TABLE_CUSTOMATTRIBUTE, i));
		if (pAttr->parent == pEnumType->tableIndex) {
			tMD_MethodDef *pCtor = MetaData_GetMethodDefFromDefRefOrSpec(pMetaData, pAttr->type, NULL, NULL);
			tMD_TypeDef *pAttrType = MetaData_GetTypeDefFromMethodDef(pCtor);
			if (strcmp(pAttrType->nameSpace, "System") == 0 && strcmp(pAttrType->name, "FlagsAttribute") == 0) {
				kind |= 1 << 16;
				break;
			}
		}
	}

	*(U32*)pReturnValue = kind;
	return NULL;
}

tAsyncCall* System_Enum_Internal_GetInfo(PTR pThis_, PTR pParams, PTR pReturnValue) {
	tMD_TypeDef *pEnumType = RuntimeType_DeRef((PTR)((tMD_TypeDef**)pParams)[0]);
	U32 i, retIndex;
	HEAP_PTR names, values;

	// An enum type always has just one non-literal field, with all other fields being the values.
	// Each array is stored into its out parameter (a slot in the caller's frame, which the collector
	// scans) as soon as it exists: the next allocation may collect, and these are otherwise held only
	// in C locals.
	names = SystemArray_NewVector(types[TYPE_SYSTEM_ARRAY_STRING], pEnumType->numFields - 1);
	*(((HEAP_PTR**)pParams)[1]) = names;
	values = SystemArray_NewVector(types[TYPE_SYSTEM_ARRAY_INT32], pEnumType->numFields - 1);
	*(((HEAP_PTR**)pParams)[2]) = values;

	for (i=0, retIndex=0; i<pEnumType->numFields; i++) {
		tMD_FieldDef *pField = pEnumType->ppFields[i];
		HEAP_PTR name;
		I32 value;

		if (!FIELD_ISLITERAL(pField)) {
			continue;
		}

		name = SystemString_FromCharPtrASCII(pField->name);
		SystemArray_StoreElement(names, retIndex, (PTR)&name);
		MetaData_GetConstant(pField->pMetaData, pField->tableIndex, (PTR)&value);
		SystemArray_StoreElement(values, retIndex, (PTR)&value);
		retIndex++;
	}

	*(((HEAP_PTR**)pParams)[1]) = names;
	*(((HEAP_PTR**)pParams)[2]) = values;

	return NULL;
}
