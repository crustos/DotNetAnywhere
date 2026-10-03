#if !LOCALTEST

using System;
using System.Runtime.CompilerServices;
using System.Collections.Generic;
using System.Text;

namespace System {
	// Enum text conversion follows the reference runtimes' rules:
	//  * a value equal to a named member prints that name;
	//  * otherwise a [Flags] enum is split into its members (largest first, zero-valued members never
	//    included), and prints the number instead if any bits are left over;
	//  * otherwise (not [Flags]) the number is printed, in the underlying type's own signedness.
	// Member names come back ordered by value. Values are handled as 32-bit words, so an enum whose
	// underlying type is 64 bits wide is only represented faithfully while its members fit in 32.
	public abstract class Enum : ValueType {

		[MethodImpl(MethodImplOptions.InternalCall)]
		extern static private void Internal_GetInfo(Type enumType, out string[] names, out int[] values);

		[MethodImpl(MethodImplOptions.InternalCall)]
		extern static private int Internal_GetKind(Type enumType);

		private static Dictionary<Type, EnumInfo> cache = new Dictionary<Type, EnumInfo>();

		private sealed class EnumInfo {
			public string[] names;
			public int[] values;      // sorted ascending as unsigned words, parallel to names
			public bool isSigned;
			public int size;          // bytes in the underlying type
			public bool isFlags;

			public static EnumInfo GetInfo(Type enumType) {
				lock (cache) {
					EnumInfo info;
					if (!Enum.cache.TryGetValue(enumType, out info)) {
						info = new EnumInfo();
						Enum.Internal_GetInfo(enumType, out info.names, out info.values);
						int kind = Enum.Internal_GetKind(enumType);
						info.isSigned = (kind & 1) != 0;
						info.size = (kind >> 8) & 0xff;
						info.isFlags = (kind & (1 << 16)) != 0;
						info.SortByValue();
						Enum.cache.Add(enumType, info);
					}
					return info;
				}
			}

			// Insertion sort (enums are small), comparing the words as unsigned so that the order is
			// that of the underlying bit patterns, which is what the reference runtimes use.
			private void SortByValue() {
				for (int i = 1; i < values.Length; i++) {
					int v = values[i];
					string n = names[i];
					int j = i - 1;
					while (j >= 0 && (uint)values[j] > (uint)v) {
						values[j + 1] = values[j];
						names[j + 1] = names[j];
						j--;
					}
					values[j + 1] = v;
					names[j + 1] = n;
				}
			}

			public string NameOf(int raw) {
				for (int i = 0; i < values.Length; i++) {
					if (values[i] == raw) {
						return names[i];
					}
				}
				return null;
			}

			public string Number(int raw) {
				return isSigned ? raw.ToString() : ((uint)raw).ToString();
			}

			public string FormatGeneral(int raw, bool forceFlags) {
				string name = NameOf(raw);
				if (name != null) {
					return name;
				}
				if (!isFlags && !forceFlags) {
					return Number(raw);
				}
				if (raw == 0) {
					return "0";
				}
				// Split into members, from the largest down; a zero member never takes part.
				int remaining = raw;
				List<string> found = new List<string>();
				for (int i = values.Length - 1; i >= 0; i--) {
					int v = values[i];
					if (v != 0 && (remaining & v) == v) {
						found.Add(names[i]);
						remaining &= ~v;
					}
				}
				if (remaining != 0) {
					return Number(raw);   // bits no member accounts for
				}
				StringBuilder sb = new StringBuilder();
				for (int i = found.Count - 1; i >= 0; i--) {
					if (sb.Length > 0) {
						sb.Append(", ");
					}
					sb.Append(found[i]);
				}
				return sb.ToString();
			}

			public string Hex(int raw, bool upper) {
				uint v = (uint)raw;
				if (size > 0 && size < 4) {
					v &= (uint)((1L << (size * 8)) - 1);
				}
				int digits = (size > 0 ? size : 4) * 2;
				string s = "";
				for (int i = 0; i < digits; i++) {
					int nib = (int)((v >> ((digits - 1 - i) * 4)) & 0xf);
					s += (char)(nib < 10 ? '0' + nib : (upper ? 'A' : 'a') + (nib - 10));
				}
				return s;
			}
		}

		protected Enum() { }

		[MethodImpl(MethodImplOptions.InternalCall)]
		extern private int Internal_GetValue();

		private static void CheckEnumType(Type enumType) {
			if (enumType == null) {
				throw new ArgumentNullException("enumType");
			}
			if (!enumType.IsEnum) {
				throw new ArgumentException("Type provided must be an Enum.", "enumType");
			}
		}

		// The word for a value given as an enum of this type, or as a bare integer
		private static int RawValue(Type enumType, object value) {
			Enum e = value as Enum;
			if (e != null) {
				if (e.GetType() != enumType) {
					throw new ArgumentException("Object must be the same type as the enum.");
				}
				return e.Internal_GetValue();
			}
			if (value is int) { return (int)value; }
			if (value is uint) { return (int)(uint)value; }
			if (value is short) { return (short)value; }
			if (value is ushort) { return (ushort)value; }
			if (value is sbyte) { return (sbyte)value; }
			if (value is byte) { return (byte)value; }
			if (value is long) { return (int)(long)value; }
			if (value is ulong) { return (int)(ulong)value; }
			throw new ArgumentException("The value passed in must be an enum base or an underlying type for an enum, such as an Int32.");
		}

		public static string[] GetNames(Type enumType) {
			CheckEnumType(enumType);
			string[] names = EnumInfo.GetInfo(enumType).names;
			string[] copy = new string[names.Length];
			for (int i = 0; i < names.Length; i++) {
				copy[i] = names[i];
			}
			return copy;
		}

		// The member's name, or null if the value is not exactly a member (a flags combination is not)
		public static string GetName(Type enumType, object value) {
			CheckEnumType(enumType);
			if (value == null) {
				throw new ArgumentNullException("value");
			}
			return EnumInfo.GetInfo(enumType).NameOf(RawValue(enumType, value));
		}

		public static bool IsDefined(Type enumType, object value) {
			return GetName(enumType, value) != null;
		}

		public static string Format(Type enumType, object value, string format) {
			CheckEnumType(enumType);
			if (value == null) {
				throw new ArgumentNullException("value");
			}
			if (format == null) {
				throw new ArgumentNullException("format");
			}
			EnumInfo info = EnumInfo.GetInfo(enumType);
			int raw = RawValue(enumType, value);
			switch (format) {
			case "G":
			case "g":
				return info.FormatGeneral(raw, false);
			case "F":
			case "f":
				return info.FormatGeneral(raw, true);
			case "D":
			case "d":
				return info.Number(raw);
			case "X":
				return info.Hex(raw, true);
			case "x":
				return info.Hex(raw, false);
			}
			throw new FormatException("Format string was not valid.");
		}

		public override string ToString() {
			return Format(this.GetType(), this, "G");
		}

	}
}

#endif
