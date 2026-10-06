using System;

// (mcs compiles `v != 0` on a long to a compare; Roslyn emits brtrue/brfalse on the int64, which CC#'s hybrid tests build with)
// brtrue/brfalse on an int64: Roslyn emits it for `if (x != 0)` / `x == 0` on a long. Both halves must be tested and the whole 8-byte
// value popped, also on a 32-bit target (where a pointer is 4 bytes).
class Int64Branch {
	static int Nz(long v) { if (v != 0) return 1; return 0; }
	static int Z(long v) { if (v == 0) return 1; return 0; }
	static long Pick(long p, long a, long b) { return p != 0 ? a : b; }
	static bool Flag(long v) { return v != 0; }

	static void Main() {
		long[] vals = { 0L, 1L, -1L, 0x100000000L, 0x7fffffffL, -0x100000000L, long.MinValue, long.MaxValue, 47344L };
		int sum = 0;
		foreach (long v in vals) {
			Console.WriteLine(v + " " + Nz(v) + " " + Z(v) + " " + Pick(v, 11L, 22L) + " " + Flag(v));
			sum += Nz(v) * 3 + Z(v);
		}
		// the stack after the branch must be intact: a second long argument and a following value
		Console.WriteLine(Pick(0x100000000L, 0x123456789L, 5L) + " " + Pick(0L, 5L, 0x123456789L));
		Console.WriteLine("sum " + sum);
	}
}
