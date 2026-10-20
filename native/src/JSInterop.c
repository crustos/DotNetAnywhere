#include "Compat.h"
#include "Sys.h"
#include "CLIFile.h"
#include "MetaData.h"
#include "Thread.h"
#include "Type.h"

#include "System.Array.h"
#include "System.RuntimeType.h"
#include "System.String.h"
#include "System.Reflection.MethodBase.h"

int JSInterop_CallDotNet(char* assemblyName, char* namespace, char* className, char* methodName, char* stringArg) {
	// TODO: Can't we reuse threads? Need to reset their state somehow.
	tThread *pThread = Thread();

	HEAP_PTR arg = SystemString_FromCharPtrASCII(stringArg);
	Heap_MakeUndeletable(arg);

	tMetaData *pAssemblyMetadata = CLIFile_GetMetaDataForLoadedAssembly(assemblyName);

	// Find any overload of the named method; assume it's the right one.
	// Specifying it exactly (type generic args, method generic args, arguments themselves, picking the
	// inherited methods if needed), is complex and not required at the moment.
	tMD_TypeDef *pTypeDef = MetaData_GetTypeDefFromName(pAssemblyMetadata, namespace, className, NULL, /* assertExists */ 1);
	MetaData_Fill_TypeDef(pTypeDef, NULL, NULL);
	for (U32 i=0; i<pTypeDef->numMethods; i++) {
		if (strcmp(pTypeDef->ppMethods[i]->name, methodName) == 0) {
			tMD_MethodDef *pMethodDef = pTypeDef->ppMethods[i];
			
			// We found the method - now call it
			Thread_SetEntryPoint(pThread, pAssemblyMetadata, pMethodDef->tableIndex, (PTR)&arg, sizeof(void*));
			Thread_HostSchedules = 1;      // the JavaScript host reschedules when every thread is sleeping
			int result = Thread_Execute();

			Heap_MakeDeletable(arg);
			return result;
		}
	}

	Crash("Found type %s, but no such method %s\n", className, methodName);
	FAKE_RETURN;
}

tAsyncCall* Framework_JSInterop_ToHeapRef(PTR pThis_, PTR pParams, PTR pReturnValue) {
	HEAP_PTR obj = ((HEAP_PTR*)pParams)[0];
	*(HEAP_PTR*)pReturnValue = obj;
	return NULL;
}

tAsyncCall* Framework_JSInterop_FromHeapRefImpl(PTR pThis_, PTR pParams, PTR pReturnValue) {
	HEAP_PTR obj = ((HEAP_PTR*)pParams)[0];
	*(HEAP_PTR*)pReturnValue = obj;
	return NULL;
}

tAsyncCall* Framework_JSInterop_Activator_CreateInstance(PTR pThis_, PTR pParams, PTR pReturnValue) {
	tMD_TypeDef *pTypeDef = RuntimeType_DeRef((PTR)((tMD_TypeDef**)pParams)[0]);
	*(HEAP_PTR*)pReturnValue = (HEAP_PTR)Heap_AllocType(pTypeDef);
	return NULL;
}

// Activator.DefaultConstructor(Type): the type's parameterless .ctor as a MethodBase (so CreateInstance<T> can Invoke it), or null
tAsyncCall* System_Activator_DefaultConstructor(PTR pThis_, PTR pParams, PTR pReturnValue) {
	tMD_TypeDef *pTypeDef = RuntimeType_DeRef((PTR)((tMD_TypeDef**)pParams)[0]);
	*(HEAP_PTR*)pReturnValue = NULL;
	MetaData_Fill_TypeDef(pTypeDef, NULL, NULL);
	for (U32 i = 0; i < pTypeDef->numMethods; i++) {
		tMD_MethodDef *pM = pTypeDef->ppMethods[i];
		if (pM != NULL && !METHOD_ISSTATIC(pM) && pM->numberOfParameters == 1 && strcmp((const char*)pM->name, ".ctor") == 0) {
			HEAP_PTR mb = Heap_AllocType(types[TYPE_SYSTEM_REFLECTION_METHODBASE]);
			*(HEAP_PTR*)pReturnValue = mb;     // (held by the return value before the next allocation, so it is not GCed)
			((tMethodBase*)mb)->methodDef = pM;
			((tMethodBase*)mb)->ownerType = Type_GetTypeObject(pTypeDef);
			((tMethodBase*)mb)->name = SystemString_FromCharPtrASCII(pM->name);
			break;
		}
	}
	return NULL;
}