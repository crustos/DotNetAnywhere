namespace System.Runtime.InteropServices {
	// Compilers turn this into a ClassLayout metadata row (Pack, Size) rather than a
	// custom attribute; the runtime reads Pack from there (see Marshal.c).
	[AttributeUsage(AttributeTargets.Class | AttributeTargets.Struct, Inherited = false)]
	public sealed class StructLayoutAttribute : Attribute {
		private LayoutKind val;
		public int Pack;
		public int Size;
		public CharSet CharSet;

		public StructLayoutAttribute(LayoutKind layoutKind) {
			this.val = layoutKind;
		}

		public StructLayoutAttribute(short layoutKind) {
			this.val = (LayoutKind)layoutKind;
		}

		public LayoutKind Value {
			get { return val; }
		}
	}
}
