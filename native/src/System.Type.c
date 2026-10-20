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

#include "System.Array.h"
#include "System.Type.h"
#include "System.Reflection.MethodInfo.h"
#include "System.Reflection.PropertyInfo.h"
#include "System.RuntimeType.h"
#include "System.String.h"

#include "Types.h"
#include "Type.h"
#include "CLIFile.h"
#include "Heap.h"
#include "MetaData.h"

tAsyncCall* System_Type_GetTypeFromHandle(PTR pThis_, PTR pParams, PTR pReturnValue) {
	tMD_TypeDef *pTypeDef = *(tMD_TypeDef**)pParams;

	*(HEAP_PTR*)pReturnValue = Type_GetTypeObject(pTypeDef);

	return NULL;
}

tAsyncCall* System_Type_get_IsValueType(PTR pThis_, PTR pParams, PTR pReturnValue) {
	tRuntimeType *pRuntimeType = (tRuntimeType*)pThis_;
	
	*(U32*)pReturnValue = pRuntimeType->pTypeDef->isValueType;

	return NULL;
}

void DotNetStringToCString(unsigned char* buf, U32 bufLength, STRING dotnetString) {
	U32 stringLen;
	STRING2 dotnetString2 = SystemString_GetString(dotnetString, &stringLen);
	if (stringLen > bufLength) {
		Crash("String of length %i was too long for buffer of length %i\n", stringLen, bufLength);
	}

	U32 i;
	for (i=0; i<stringLen; i++) {
		buf[i] = (unsigned char)dotnetString2[i];
	}
	buf[i] = 0;
}

tAsyncCall* System_Type_EnsureAssemblyLoaded(PTR pThis_, PTR pParams, PTR pReturnValue) {
	unsigned char assemblyName[256];
	DotNetStringToCString(assemblyName, 256, ((HEAP_PTR*)pParams)[0]);
	CLIFile_GetMetaDataForAssembly(assemblyName);

	*(HEAP_PTR*)pReturnValue = NULL;
	return NULL;
}

tAsyncCall* System_Type_GetTypeFromName(PTR pThis_, PTR pParams, PTR pReturnValue) {
	unsigned char namespaceName[256];
	unsigned char className[256];
	tMD_TypeDef *pTypeDef;

	DotNetStringToCString(namespaceName, 256, ((HEAP_PTR*)pParams)[1]);
	DotNetStringToCString(className, 256, ((HEAP_PTR*)pParams)[2]);

	if (((HEAP_PTR*)pParams)[0] == 0) {
		// assemblyName is null, so search all loaded assemblies
		pTypeDef = CLIFile_FindTypeInAllLoadedAssemblies(namespaceName, className);
	} else {
		// assemblyName is specified
		unsigned char assemblyName[256];
		DotNetStringToCString(assemblyName, 256, ((HEAP_PTR*)pParams)[0]);
		tMetaData *pAssemblyMetadata = CLIFile_GetMetaDataForAssembly(assemblyName);
		pTypeDef = MetaData_GetTypeDefFromName(pAssemblyMetadata, namespaceName, className, NULL, /* assertExists */ 1);
	}

	MetaData_Fill_TypeDef(pTypeDef, NULL, NULL);
	*(HEAP_PTR*)pReturnValue = Type_GetTypeObject(pTypeDef);
	return NULL;
}

tAsyncCall* System_Type_GetProperties(PTR pThis_, PTR pParams, PTR pReturnValue) {
	tRuntimeType *pRuntimeType = (tRuntimeType*)pThis_;
	tMD_TypeDef *pTypeDef = pRuntimeType->pTypeDef;
	tMetaData *pMetaData = pTypeDef->pMetaData;

	// First we search through the table of propertymaps to find the propertymap for the requested type
	U32 i;
	IDX_TABLE firstIdx = 0, lastIdxExc = 0;
	U32 numPropertyRows = pMetaData->tables.numRows[MD_TABLE_PROPERTY];
	U32 numPropertymapRows = pMetaData->tables.numRows[MD_TABLE_PROPERTYMAP];
	for (i=1; i <= numPropertymapRows; i++) {
		tMD_PropertyMap *pPropertyMap = (tMD_PropertyMap*)MetaData_GetTableRow(pMetaData, MAKE_TABLE_INDEX(MD_TABLE_PROPERTYMAP, i));
		if (pPropertyMap->parent == pTypeDef->tableIndex) {
			firstIdx = TABLE_OFS(pPropertyMap->propertyList);
			if (i < numPropertymapRows) {
				tMD_PropertyMap *pNextPropertyMap = (tMD_PropertyMap*)MetaData_GetTableRow(pMetaData, MAKE_TABLE_INDEX(MD_TABLE_PROPERTYMAP, i + 1));
				lastIdxExc = TABLE_OFS(pNextPropertyMap->propertyList);
			} else {
				lastIdxExc = numPropertyRows + 1;
			}
			break;
		}
	}

	// Instantiate a PropertyInfo[]
	U32 numProperties = lastIdxExc - firstIdx;
	tMD_TypeDef *pArrayType = Type_GetArrayTypeDef(types[TYPE_SYSTEM_REFLECTION_PROPERTYINFO], NULL, NULL);
	HEAP_PTR ret = SystemArray_NewVector(pArrayType, numProperties);
	// Allocate to return value straight away, so it cannot be GCed
	*(HEAP_PTR*)pReturnValue = ret;

	// Now fill the PropertyInfo[]
	for (i=0; i<numProperties; i++) {
		IDX_TABLE index = MAKE_TABLE_INDEX(MD_TABLE_PROPERTY, firstIdx + i);
		tMD_Property *pPropertyMetadata = (tMD_Property*)MetaData_GetTableRow(pMetaData, index);

		// Instantiate PropertyInfo and put it in the array
		tPropertyInfo *pPropertyInfo = (tPropertyInfo*)Heap_AllocType(types[TYPE_SYSTEM_REFLECTION_PROPERTYINFO]);
		SystemArray_StoreElement(ret, i, (PTR)&pPropertyInfo);

		// Assign ownerType
		pPropertyInfo->ownerType = pThis_;

		// Assign name
		pPropertyInfo->name = SystemString_FromCharPtrASCII(pPropertyMetadata->name);

		// Assign propertyType
		U32 sigLength;
		PTR typeSig = MetaData_GetBlob(pPropertyMetadata->typeSig, &sigLength);
		MetaData_DecodeSigEntry(&typeSig); // Ignored: prolog
		MetaData_DecodeSigEntry(&typeSig); // Ignored: number of 'getter' parameters		
		tMD_TypeDef *propertyTypeDef = Type_GetTypeFromSig(pMetaData, &typeSig, NULL, NULL);
		MetaData_Fill_TypeDef(propertyTypeDef, NULL, NULL);
		pPropertyInfo->propertyType = Type_GetTypeObject(propertyTypeDef);

		// Assign propertyMetadata
		pPropertyInfo->index = index;
		pPropertyInfo->pMetaData = pPropertyMetadata;
	}

	return NULL;
}

// The type's own fields (not inherited ones), as tMD_FieldDef pointers.
tAsyncCall* System_Type_Internal_GetFields(PTR pThis_, PTR pParams, PTR pReturnValue) {
	tMD_TypeDef *pTypeDef = ((tRuntimeType*)pThis_)->pTypeDef;
	U32 i;
	MetaData_Fill_TypeDef(pTypeDef, NULL, NULL);
	HEAP_PTR ret = SystemArray_NewVector(Type_GetArrayTypeDef(types[TYPE_SYSTEM_INTPTR], NULL, NULL), pTypeDef->numFields);
	*(HEAP_PTR*)pReturnValue = ret;
	for (i=0; i<pTypeDef->numFields; i++) {
		SystemArray_StoreElement(ret, i, (PTR)&pTypeDef->ppFields[i]);
	}
	return NULL;
}

tAsyncCall* System_Type_Internal_GetNestedType(PTR pThis_, PTR pParams, PTR pReturnValue) {
	tMD_TypeDef *pOuter = ((tRuntimeType*)pThis_)->pTypeDef;
	unsigned char name[256];
	DotNetStringToCString(name, 255, ((HEAP_PTR*)pParams)[0]);
	tMD_TypeDef *pTypeDef = MetaData_GetTypeDefFromName(pOuter->pMetaData, (STRING)"", name, pOuter, 0);
	if (pTypeDef != NULL) MetaData_Fill_TypeDef(pTypeDef, NULL, NULL);
	*(HEAP_PTR*)pReturnValue = pTypeDef == NULL ? NULL : Type_GetTypeObject(pTypeDef);
	return NULL;
}

tAsyncCall* System_Type_Internal_FieldName(PTR pThis_, PTR pParams, PTR pReturnValue) {
	*(HEAP_PTR*)pReturnValue = SystemString_FromCharPtrASCII((*(tMD_FieldDef**)pParams)->name);
	return NULL;
}

tAsyncCall* System_Type_Internal_FieldType(PTR pThis_, PTR pParams, PTR pReturnValue) {
	tMD_FieldDef *pField = *(tMD_FieldDef**)pParams;
	MetaData_Fill_TypeDef(pField->pType, NULL, NULL);
	*(HEAP_PTR*)pReturnValue = Type_GetTypeObject(pField->pType);
	return NULL;
}

tAsyncCall* System_Type_Internal_FieldFlags(PTR pThis_, PTR pParams, PTR pReturnValue) {
	*(I32*)pReturnValue = (*(tMD_FieldDef**)pParams)->flags;
	return NULL;
}

static PTR FieldAddress(tMD_FieldDef *pField, HEAP_PTR obj) {
	return FIELD_ISSTATIC(pField) ? pField->pMemory : (PTR)obj + pField->memOffset;
}

// object GetValue(IntPtr field, object obj): obj is a reference object or a box, whose data both start at the heap pointer
tAsyncCall* System_Type_Internal_GetFieldValue(PTR pThis_, PTR pParams, PTR pReturnValue) {
	tMD_FieldDef *pField = *(tMD_FieldDef**)pParams;
	if (FIELD_ISLITERAL(pField)) {
		// pMemory is the constant's metadata: its blob is a length, then the value, little-endian (a string's in UTF-16)
		tMD_Constant *pConst = (tMD_Constant*)pField->pMemory;
		unsigned char *blob = pConst->value;
		U32 len = MetaData_DecodeHeapEntryLength(&blob);
		if (pConst->type == ELEMENT_TYPE_STRING) {
			*(HEAP_PTR*)pReturnValue = SystemString_FromUTF16(blob, len >> 1);
		} else if (!pField->pType->isValueType) {
			*(HEAP_PTR*)pReturnValue = NULL;
		} else {
			HEAP_PTR boxed = Heap_AllocType(pField->pType);
			memcpy(boxed, blob, len < pField->pType->instanceMemSize ? len : pField->pType->instanceMemSize);
			*(HEAP_PTR*)pReturnValue = boxed;
		}
		return NULL;
	}
	PTR addr = FieldAddress(pField, *(HEAP_PTR*)(pParams + sizeof(void*)));
	*(HEAP_PTR*)pReturnValue = pField->pType->isValueType ? Heap_Box(pField->pType, addr) : *(HEAP_PTR*)addr;
	return NULL;
}

// void SetValue(IntPtr field, object obj, object value): the caller has checked that a boxed value is the field's type
tAsyncCall* System_Type_Internal_SetFieldValue(PTR pThis_, PTR pParams, PTR pReturnValue) {
	tMD_FieldDef *pField = *(tMD_FieldDef**)pParams;
	PTR addr = FieldAddress(pField, *(HEAP_PTR*)(pParams + sizeof(void*)));
	HEAP_PTR value = *(HEAP_PTR*)(pParams + 2 * sizeof(void*));
	if (!pField->pType->isValueType) *(HEAP_PTR*)addr = value;
	else if (value == NULL) memset(addr, 0, pField->memSize);
	else memcpy(addr, value, pField->memSize);
	return NULL;
}

// The type's static constructor, for the caller to run, when the type is not initialised yet (it is marked initialised
// now, as the JIT does before it calls one); otherwise null.
tAsyncCall* System_Type_Internal_TakeTypeInitializer(PTR pThis_, PTR pParams, PTR pReturnValue) {
	tMD_TypeDef *pTypeDef = ((tRuntimeType*)pThis_)->pTypeDef;
	*(HEAP_PTR*)pReturnValue = NULL;
	if (pTypeDef->isTypeInitialised) return NULL;
	pTypeDef->isTypeInitialised = 1;
	if (pTypeDef->pStaticConstructor == NULL) return NULL;
	tMethodInfo *pMethodInfo = (tMethodInfo*)Heap_AllocType(types[TYPE_SYSTEM_REFLECTION_METHODINFO]);
	*(HEAP_PTR*)pReturnValue = (HEAP_PTR)pMethodInfo;
	pMethodInfo->methodBase.ownerType = pThis_;
	pMethodInfo->methodBase.name = SystemString_FromCharPtrASCII(pTypeDef->pStaticConstructor->name);
	pMethodInfo->methodBase.methodDef = pTypeDef->pStaticConstructor;
	return NULL;
}

// The type's TypeAttributes (Serializable 0x2000, Abstract 0x80, Interface 0x20, ...).
tAsyncCall* System_Type_Internal_TypeFlags(PTR pThis_, PTR pParams, PTR pReturnValue) {
	*(I32*)pReturnValue = ((tRuntimeType*)pThis_)->pTypeDef->flags;
	return NULL;
}

// Has the field a custom attribute of exactly this type?  (Only its type is looked at: nothing is constructed.)
tAsyncCall* System_Type_Internal_FieldHasAttribute(PTR pThis_, PTR pParams, PTR pReturnValue) {
	tMD_FieldDef *pField = *(tMD_FieldDef**)pParams;
	tMD_TypeDef *pAttr = ((tRuntimeType*)*(HEAP_PTR*)(pParams + sizeof(void*)))->pTypeDef;
	tMetaData *pMetaData = pField->pMetaData;
	U32 i, rows = pMetaData->tables.numRows[MD_TABLE_CUSTOMATTRIBUTE];
	*(U32*)pReturnValue = 0;
	for (i = 1; i <= rows; i++) {
		tMD_CustomAttribute *pCA = (tMD_CustomAttribute*)MetaData_GetTableRow(pMetaData, MAKE_TABLE_INDEX(MD_TABLE_CUSTOMATTRIBUTE, i));
		if (pCA->parent != pField->tableIndex) continue;
		tMD_MethodDef *pCtor = MetaData_GetMethodDefFromDefRefOrSpec(pMetaData, pCA->type, NULL, NULL);
		if (pCtor != NULL && MetaData_GetTypeDefFromMethodDef(pCtor) == pAttr) { *(U32*)pReturnValue = 1; return NULL; }
	}
	return NULL;
}

// A zeroed instance, no constructor run (FormatterServices.GetUninitializedObject); a value type comes back boxed.
tAsyncCall* System_Type_Internal_NewObject(PTR pThis_, PTR pParams, PTR pReturnValue) {
	tMD_TypeDef *pTypeDef = ((tRuntimeType*)*(HEAP_PTR*)pParams)->pTypeDef;
	MetaData_Fill_TypeDef(pTypeDef, NULL, NULL);
	*(HEAP_PTR*)pReturnValue = Heap_AllocType(pTypeDef);
	return NULL;
}

// The type's own public methods (not constructors, not inherited ones), as MethodInfo[]
tAsyncCall* System_Type_Internal_GetMethods(PTR pThis_, PTR pParams, PTR pReturnValue) {
	tMD_TypeDef *pTypeDef = ((tRuntimeType*)pThis_)->pTypeDef;
	U32 i, n = 0;
	MetaData_Fill_TypeDef(pTypeDef, NULL, NULL);
	for (i=0; i<pTypeDef->numMethods; i++) {
		if (pTypeDef->ppMethods[i] != NULL && pTypeDef->ppMethods[i]->name[0] != '.' && (pTypeDef->ppMethods[i]->flags & 7) == 6) n++;
	}
	HEAP_PTR ret = SystemArray_NewVector(Type_GetArrayTypeDef(types[TYPE_SYSTEM_REFLECTION_METHODINFO], NULL, NULL), n);
	*(HEAP_PTR*)pReturnValue = ret;
	n = 0;
	for (i=0; i<pTypeDef->numMethods; i++) {
		tMD_MethodDef *pMethodDef = pTypeDef->ppMethods[i];
		if (pMethodDef == NULL || pMethodDef->name[0] == '.' || (pMethodDef->flags & 7) != 6) continue;
		tMethodInfo *pMethodInfo = (tMethodInfo*)Heap_AllocType(types[TYPE_SYSTEM_REFLECTION_METHODINFO]);
		SystemArray_StoreElement(ret, n++, (PTR)&pMethodInfo);
		pMethodInfo->methodBase.ownerType = pThis_;
		pMethodInfo->methodBase.name = SystemString_FromCharPtrASCII(pMethodDef->name);
		pMethodInfo->methodBase.methodDef = pMethodDef;
	}
	return NULL;
}

tAsyncCall* System_Type_GetMethod(PTR pThis_, PTR pParams, PTR pReturnValue)
{
	// Read param
	unsigned char methodName[256];
	DotNetStringToCString(methodName, 256, ((HEAP_PTR*)pParams)[0]);

	// Get metadata for the 'this' type
	tRuntimeType *pRuntimeType = (tRuntimeType*)pThis_;
	tMD_TypeDef *pTypeDef = pRuntimeType->pTypeDef;

	// Search for the method by name
	for (U32 i=0; i<pTypeDef->numMethods; i++) {
		if (strcmp(pTypeDef->ppMethods[i]->name, methodName) == 0) {
			tMD_MethodDef *pMethodDef = pTypeDef->ppMethods[i];

			// Instantiate a MethodInfo
			tMethodInfo *pMethodInfo = (tMethodInfo*)Heap_AllocType(types[TYPE_SYSTEM_REFLECTION_METHODINFO]);

			// Assign ownerType
			pMethodInfo->methodBase.ownerType = pThis_;

			// Assign name
			pMethodInfo->methodBase.name = SystemString_FromCharPtrASCII(pMethodDef->name);

			// Assign method def
			pMethodInfo->methodBase.methodDef = pMethodDef;

			*(HEAP_PTR*)pReturnValue = (HEAP_PTR)pMethodInfo;
			return NULL;
		}
	}

	// Not found
	*(HEAP_PTR*)pReturnValue = NULL;
	return NULL;
}
