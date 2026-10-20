#if !LOCALTEST

using System.Runtime.CompilerServices;
namespace System {
	public static class BitConverter {

		public static readonly bool IsLittleEndian = AmILittleEndian();

		private unsafe static bool AmILittleEndian() {
			int i = 1;
			byte b = *((byte*)&i);
			return (b == 1);
		}

		public unsafe static long DoubleToInt64Bits(double value) {
			return *((long*)&value);
		}

		public unsafe static double Int64BitsToDouble(long value) {
			return *((double*)&value);
		}

		public unsafe static int SingleToInt32Bits(float value) {
			return *((int*)&value);
		}

		public unsafe static float Int32BitsToSingle(int value) {
			return *((float*)&value);
		}

		// In the machine's byte order, as in .NET.
		static byte[] Bytes(long value, int size) {
			byte[] b = new byte[size];
			for (int i = 0; i < size; i++) b[IsLittleEndian ? i : size - 1 - i] = (byte)(value >> (8 * i));
			return b;
		}

		static long Read(byte[] value, int startIndex, int size) {
			if (value == null) throw new ArgumentNullException("value");
			if (startIndex < 0 || startIndex + size > value.Length) throw new ArgumentOutOfRangeException("startIndex");
			long r = 0;
			for (int i = 0; i < size; i++) r |= (long)value[startIndex + (IsLittleEndian ? i : size - 1 - i)] << (8 * i);
			return r;
		}

		public static byte[] GetBytes(bool value) { return Bytes(value ? 1 : 0, 1); }
		public static byte[] GetBytes(char value) { return Bytes(value, 2); }
		public static byte[] GetBytes(short value) { return Bytes(value, 2); }
		public static byte[] GetBytes(ushort value) { return Bytes(value, 2); }
		public static byte[] GetBytes(int value) { return Bytes(value, 4); }
		public static byte[] GetBytes(uint value) { return Bytes(value, 4); }
		public static byte[] GetBytes(long value) { return Bytes(value, 8); }
		public static byte[] GetBytes(ulong value) { return Bytes((long)value, 8); }
		public static byte[] GetBytes(float value) { return Bytes(SingleToInt32Bits(value), 4); }
		public static byte[] GetBytes(double value) { return Bytes(DoubleToInt64Bits(value), 8); }

		public static bool ToBoolean(byte[] value, int startIndex) { return Read(value, startIndex, 1) != 0; }
		public static char ToChar(byte[] value, int startIndex) { return (char)Read(value, startIndex, 2); }
		public static short ToInt16(byte[] value, int startIndex) { return (short)Read(value, startIndex, 2); }
		public static ushort ToUInt16(byte[] value, int startIndex) { return (ushort)Read(value, startIndex, 2); }
		public static int ToInt32(byte[] value, int startIndex) { return (int)Read(value, startIndex, 4); }
		public static uint ToUInt32(byte[] value, int startIndex) { return (uint)Read(value, startIndex, 4); }
		public static long ToInt64(byte[] value, int startIndex) { return Read(value, startIndex, 8); }
		public static ulong ToUInt64(byte[] value, int startIndex) { return (ulong)Read(value, startIndex, 8); }
		public static float ToSingle(byte[] value, int startIndex) { return Int32BitsToSingle((int)Read(value, startIndex, 4)); }
		public static double ToDouble(byte[] value, int startIndex) { return Int64BitsToDouble(Read(value, startIndex, 8)); }

	}
}

#endif
