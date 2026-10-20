#if !LOCALTEST

namespace System {
	public class FieldAccessException : SystemException {
		public FieldAccessException() : base("Attempted to access a field that is not accessible by the caller.") { }
		public FieldAccessException(string message) : base(message) { }
	}
}

namespace System.Reflection {
	[Flags]
	public enum BindingFlags {
		Default = 0, IgnoreCase = 1, DeclaredOnly = 2, Instance = 4, Static = 8, Public = 16, NonPublic = 32, FlattenHierarchy = 64,
	}

	public sealed class FieldInfo : MemberInfo {
		const int AccessMask = 0x7, PublicAccess = 0x6, StaticFlag = 0x10, InitOnlyFlag = 0x20, LiteralFlag = 0x40;
		readonly IntPtr handle;
		readonly int flags;

		internal FieldInfo(Type declaringType, IntPtr field) : base(declaringType, Type.Internal_FieldName(field)) {
			this.handle = field;
			this.flags = Type.Internal_FieldFlags(field);
		}

		public Type DeclaringType => _ownerType;
		public Type FieldType => Type.Internal_FieldType(handle);
		public bool IsStatic => (flags & StaticFlag) != 0;
		public bool IsPublic => (flags & AccessMask) == PublicAccess;
		public bool IsPrivate => (flags & AccessMask) == 1;
		public bool IsInitOnly => (flags & InitOnlyFlag) != 0;
		public bool IsLiteral => (flags & LiteralFlag) != 0;
		public bool IsNotSerialized => (flags & 0x80) != 0;

		public object GetValue(object obj) {
			if (!IsStatic && obj == null) throw new ArgumentNullException("obj");
			if (IsStatic) _ownerType.EnsureInitialized();
			return Type.Internal_GetFieldValue(handle, obj);
		}

		public void SetValue(object obj, object value) {
			if (IsLiteral) throw new FieldAccessException("Cannot set a constant field.");
			if (!IsStatic && obj == null) throw new ArgumentNullException("obj");
			Type t = FieldType;
			if (value != null && t.IsValueType && value.GetType() != t && !t.IsEnum) {
				throw new ArgumentException("Object of type '" + value.GetType().FullName + "' cannot be converted to type '" + t.FullName + "'.");
			}
			if (IsStatic) _ownerType.EnsureInitialized();
			Type.Internal_SetFieldValue(handle, obj, value);
		}

		public bool IsDefined(Type attributeType, bool inherit) {
			if (attributeType == null) throw new ArgumentNullException("attributeType");
			return Type.Internal_FieldHasAttribute(handle, attributeType);
		}

		public override string ToString() {
			return FieldType.Name + " " + Name;
		}
	}
}

#endif
