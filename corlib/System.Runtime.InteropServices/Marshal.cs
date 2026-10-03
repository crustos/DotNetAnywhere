using System.Runtime.CompilerServices;

namespace System.Runtime.InteropServices {

	// Only the part of Marshal that applies to *unmanaged* types: primitives, enums and
	// structs of those. Sizes are the reference runtime's LayoutKind.Sequential layout on
	// a 64-bit machine (natural alignment, reduced by Pack), not DNA's in-memory layout.
	//
	// Unlike the reference Marshal, bool is 1 byte and char is 2 (what Unsafe.SizeOf and C
	// give), not the 4 and 1 of the Win32 BOOL / ANSI default.
	public static class Marshal {

		[MethodImpl(MethodImplOptions.InternalCall)]
		private extern static int SizeOfImpl(Type t);

		public static int SizeOf(Type t) {
			if (object.ReferenceEquals(t, null)) {
				throw new ArgumentNullException("t");
			}
			int size = SizeOfImpl(t);
			if (size < 0) {
				throw new ArgumentException("Type '" + t.Name + "' cannot be marshaled as an unmanaged structure; no meaningful size or offset can be computed.");
			}
			return size;
		}

		public static int SizeOf(object structure) {
			if (structure == null) {
				throw new ArgumentNullException("structure");
			}
			return SizeOf(structure.GetType());
		}

		public static int SizeOf<T>() {
			return SizeOf(typeof(T));
		}
	}
}
