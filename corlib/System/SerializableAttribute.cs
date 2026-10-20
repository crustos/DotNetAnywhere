#if !LOCALTEST

namespace System {
	[AttributeUsage(AttributeTargets.Class | AttributeTargets.Struct | AttributeTargets.Enum | AttributeTargets.Delegate, Inherited = false)]
	public sealed class SerializableAttribute : Attribute { }

	[AttributeUsage(AttributeTargets.Field, Inherited = false)]
	public sealed class NonSerializedAttribute : Attribute { }
}

namespace System.Runtime.InteropServices {
	public sealed class ComVisibleAttribute : Attribute {
		public ComVisibleAttribute(bool visibility) { }
	}
}

#endif
