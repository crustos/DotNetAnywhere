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

#include "System.Runtime.CompilerServices.RuntimeHelpers.h"

#include "MetaData.h"
#include "Types.h"
#include "Heap.h"
#include "Type.h"
#include "System.Array.h"

tAsyncCall* System_Runtime_CompilerServices_InitializeArray(PTR pThis_, PTR pParams, PTR pReturnValue) {
	HEAP_PTR pArray;
	PTR pRawData;
	tMD_TypeDef *pArrayTypeDef;
	PTR pElements;
	U32 arrayLength;

	pArray = ((HEAP_PTR*)pParams)[0];
	pRawData = ((PTR*)pParams)[1];
	pArrayTypeDef = Heap_GetType(pArray);
	arrayLength = SystemArray_GetLength(pArray);
	pElements = SystemArray_GetElements(pArray);
	{
		// The compiler's data for the elements has each at its natural size: an enum based on a byte, a short ... is 1, 2 ... bytes. A DNA array
		// of enums has them 4 bytes each (the size of the enum's slot), so such data is widened, element by element, as the underlying type says.
		tMD_TypeDef *pElemType = pArrayTypeDef->pArrayElementType;
		U32 elemSize = pElemType->arrayElementSize, natural = elemSize, i, isSigned = 0;
		if (pElemType->pParent == types[TYPE_SYSTEM_ENUM]) {
			for (i = 0; i < pElemType->numFields; i++) {
				tMD_FieldDef *pField = pElemType->ppFields[i];
				tMD_TypeDef *pUnder;
				if (FIELD_ISSTATIC(pField)) continue;
				pUnder = pField->pType;
				if (pUnder == types[TYPE_SYSTEM_BYTE] || pUnder == types[TYPE_SYSTEM_BOOLEAN]) natural = 1;
				else if (pUnder == types[TYPE_SYSTEM_SBYTE]) { natural = 1; isSigned = 1; }
				else if (pUnder == types[TYPE_SYSTEM_UINT16] || pUnder == types[TYPE_SYSTEM_CHAR]) natural = 2;
				else if (pUnder == types[TYPE_SYSTEM_INT16]) { natural = 2; isSigned = 1; }
				break;
			}
		}
		if (natural < elemSize) {
			for (i = 0; i < arrayLength; i++) {
				const U8 *src = (const U8*)pRawData + i * natural;
				I32 v = natural == 1 ? (isSigned ? (I32)(signed char)src[0] : (I32)src[0]) : (isSigned ? (I32)(short)(src[0] | (src[1] << 8)) : (I32)(src[0] | (src[1] << 8)));   // (signed char, not I8: char is unsigned on wasm32)
				memcpy(pElements + i * elemSize, &v, sizeof(v));
			}
		} else {
			memcpy(pElements, pRawData, elemSize * arrayLength);
		}
	}

	return NULL;
}
