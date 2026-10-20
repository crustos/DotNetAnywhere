#if !LOCALTEST

namespace System.Runtime.Serialization {
	public static class FormatterServices {
		/// <summary>A zeroed instance of the type; no constructor runs.</summary>
		public static object GetUninitializedObject(Type type) {
			if (type == null) throw new ArgumentNullException("type");
			return Type.Internal_NewObject(type);
		}
	}

	public class SerializationException : SystemException {
		public SerializationException() : base("Serialization error.") { }
		public SerializationException(string message) : base(message) { }
		public SerializationException(string message, Exception innerException) : base(message, innerException) { }
	}
}

#endif
