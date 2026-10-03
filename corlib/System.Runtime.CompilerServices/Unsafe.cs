using System.Runtime.InteropServices;

namespace System.Runtime.CompilerServices {

	// Just SizeOf; the rest of Unsafe needs managed pointers, which DNA does not have.
	public static class Unsafe {
		public static int SizeOf<T>() {
			return Marshal.SizeOf(typeof(T));
		}
	}
}
