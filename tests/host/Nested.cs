// A managed program that calls C (nested.c), which calls managed code (Managed.cs) again.
using System;
using System.Runtime.InteropServices;
class NestedMain {
	[DllImport("nested")] static extern int native_twice(int x);
	[DllImport("nested")] static extern int native_sumsq(int n);
	[DllImport("nested")] static extern int native_greet_len(string s);
	[DllImport("nested")] static extern int native_depth(int n);
	[DllImport("nested")] static extern void native_fill(int[] a, int n, int v);
	[DllImport("nested")] static extern int native_churn(int rounds);

	public static int Recur(int n) { return native_depth(n); }

	static int Main() {
		Console.WriteLine("twice " + native_twice(21));
		Console.WriteLine("sumsq " + native_sumsq(6));
		Console.WriteLine("greet " + native_greet_len("h\u00e9llo"));
		Console.WriteLine("depth " + native_depth(5));
		int[] a = new int[4];
		native_fill(a, 4, 100);
		Console.WriteLine("fill " + a[0] + "," + a[1] + "," + a[2] + "," + a[3]);
		Console.WriteLine("churn " + native_churn(500));
		// a loop hot enough to be compiled into a native block, where the call to C is a stencil rather than an interpreter op
		int sum = 0;
		for (int i = 0; i < 20000; i++) sum += native_twice(i & 7);
		Console.WriteLine("loop " + sum);
		Console.WriteLine("after " + native_twice(1));
		return 0;
	}
}
