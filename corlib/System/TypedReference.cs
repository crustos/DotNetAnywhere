namespace System {
	// A managed pointer together with the type it points at: what mkrefany builds, and what
	// refanyval / refanytype take apart (C#'s undocumented __makeref / __refvalue / __reftype).
	// The runtime builds and reads the two fields directly, in this order.
	public struct TypedReference {
		internal IntPtr Value;   // the address
		internal IntPtr Type;    // the type's runtime handle
	}
}
