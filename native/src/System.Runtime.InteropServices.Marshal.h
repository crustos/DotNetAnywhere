#if !defined(__SYSTEM_RUNTIME_INTEROPSERVICES_MARSHAL_H)
#define __SYSTEM_RUNTIME_INTEROPSERVICES_MARSHAL_H

#include "Types.h"
#include "MetaData.h"

// Layout/serialisation of *unmanaged* value types (primitives, enums and structs
// of those, all the way down) in the byte layout the reference .NET runtime gives
// LayoutKind.Sequential on a 64-bit little-endian machine: natural alignment,
// reduced by [StructLayout(Pack = n)].
//
// This cannot be a memcpy of the object. DNA stores every small field in a
// 4-byte stack slot with no padding, so its in-memory image is not that layout.

// Layout size of an unmanaged value type (the `sizeof` instruction), or -1 if it is not one.
I32 Marshal_GetUnmanagedSize(tMD_TypeDef *pType);

// int SizeOfImpl(Type t)  -- byte size, or -1 if t is not an unmanaged type.
tAsyncCall* System_Runtime_InteropServices_Marshal_SizeOfImpl(PTR pThis_, PTR pParams, PTR pReturnValue);
// int Serialize(Type t, object boxed, byte[] dst, int offset)
//   0 ok, 1 not unmanaged, 2 destination too small, 3 unsupported host.
tAsyncCall* System_Runtime_InteropServices_MemoryMarshal_Serialize(PTR pThis_, PTR pParams, PTR pReturnValue);
// object Deserialize(Type t, byte[] src, int offset)  -- boxed value, or null on error.
tAsyncCall* System_Runtime_InteropServices_MemoryMarshal_Deserialize(PTR pThis_, PTR pParams, PTR pReturnValue);

#endif
