using System.Runtime.CompilerServices;

namespace System.Runtime.InteropServices {

	// The byte-copy serialisation of unmanaged structs (see Marshal.c for the layout).
	//
	// Differences from the reference runtime, all because DNA has no managed pointers:
	//  * CreateSpan(ref x, 1) returns a span over a *copy* of x, and AsBytes / Cast return
	//    copies, not views. Reading them (the `AsBytes(CreateSpan(ref x, 1)).ToArray()`
	//    idiom) is exact; writing through them does not reach x.
	//  * CreateSpan with a length other than 1 is not supported.
	public static class MemoryMarshal {

		[MethodImpl(MethodImplOptions.InternalCall)]
		private extern static int Serialize(Type t, object boxed, byte[] destination, int offset);

		[MethodImpl(MethodImplOptions.InternalCall)]
		private extern static object Deserialize(Type t, byte[] source, int offset);

		public static Span<T> CreateSpan<T>(ref T reference, int length) {
			if (length != 1) {
				throw new NotSupportedException("MemoryMarshal.CreateSpan: only a length of 1 is supported (no managed pointers).");
			}
			T[] copy = new T[1];
			copy[0] = reference;
			return new Span<T>(copy);
		}

		private static void WriteElement(Type t, object boxed, byte[] dst, int offset) {
			int status = Serialize(t, boxed, dst, offset);
			if (status == 1) {
				throw new ArgumentException("Type '" + t.Name + "' contains references and cannot be serialised as bytes.");
			}
			if (status == 2) {
				throw new ArgumentOutOfRangeException("destination");
			}
			if (status != 0) {
				throw new NotSupportedException("MemoryMarshal needs a little-endian host.");
			}
		}

		public static Span<byte> AsBytes<T>(Span<T> span) where T : struct {
			int size = Marshal.SizeOf(typeof(T));
			byte[] bytes = new byte[size * span.Length];
			for (int i = 0; i < span.Length; i++) {
				WriteElement(typeof(T), (object)span[i], bytes, i * size);
			}
			return new Span<byte>(bytes);
		}

		public static ReadOnlySpan<byte> AsBytes<T>(ReadOnlySpan<T> span) where T : struct {
			int size = Marshal.SizeOf(typeof(T));
			byte[] bytes = new byte[size * span.Length];
			for (int i = 0; i < span.Length; i++) {
				WriteElement(typeof(T), (object)span[i], bytes, i * size);
			}
			return new ReadOnlySpan<byte>(bytes);
		}

		public static T Read<T>(ReadOnlySpan<byte> source) where T : struct {
			int size = Marshal.SizeOf(typeof(T));
			if (source.Length < size) {
				throw new ArgumentOutOfRangeException("source");
			}
			return (T)Deserialize(typeof(T), source._array, source._start);
		}

		public static void Write<T>(Span<byte> destination, ref T value) where T : struct {
			int size = Marshal.SizeOf(typeof(T));
			if (destination.Length < size) {
				throw new ArgumentOutOfRangeException("destination");
			}
			WriteElement(typeof(T), (object)value, destination._array, destination._start);
		}

		// Reinterpret a span of one unmanaged type as another, through its bytes. A copy.
		public static Span<TTo> Cast<TFrom, TTo>(Span<TFrom> span) where TFrom : struct where TTo : struct {
			Span<byte> bytes = AsBytes<TFrom>(span);
			int toSize = Marshal.SizeOf(typeof(TTo));
			int count = bytes.Length / toSize;
			TTo[] result = new TTo[count];
			for (int i = 0; i < count; i++) {
				result[i] = (TTo)Deserialize(typeof(TTo), bytes._array, i * toSize);
			}
			return new Span<TTo>(result);
		}
	}
}
