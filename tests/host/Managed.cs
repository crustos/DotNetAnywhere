// The managed side of tests/host: static methods that a C host calls through native/src/Host.h.
// The same file is compiled for Mono with Oracle.cs, which calls them and prints what host.c prints.
namespace T {
	// An object that native code holds by handle.  `hist` is a child object: it stays alive only because the Counter is.
	public class Counter {
		int n;
		int[] hist;
		string tag = "";
		public Counter(int start) { n = start; hist = new int[8]; }
		public int Add(int k) { n += k; hist[k & 7] += 1; return n; }
		public int Hist(int i) { return hist[i]; }
		public string Tag(string s) { string old = tag; tag = s; return old + ">" + s; }
		public int Total { get { return n; } }
	}

	public static class Calc {
		public static Counter NewCounter(int s) { return new Counter(s); }
		public static int Bump(Counter c, int k) { return c.Add(k); }
		public static Counter Same(Counter c) { return c; }
		public static bool IsNull(Counter c) { return c == null; }
		public static int CTotal(Counter c) { return c.Total; }
		public static int CHist(Counter c, int i) { return c.Hist(i); }
		public static string CTag(Counter c, string s) { return c.Tag(s); }

		public static int AddI(int a, int b) { return a + b; }
		public static long MulL(long a, long b) { return a * b; }
		public static float ScaleF(float a, float b) { return a * b; }
		public static double Hyp(double a, double b) { return System.Math.Sqrt(a * a + b * b); }
		public static bool IsPos(int a) { return a > 0; }
		public static byte Low(int a) { return (byte)a; }
		public static short Narrow(int a) { return (short)a; }
		public static uint UClamp(uint v, uint lo, uint hi) { return v < lo ? lo : v > hi ? hi : v; }
		public static int Mixed(int a, double b, long c, float d, int e) { return a + (int)b + (int)c + (int)d + e; }
		public static void Nop() { }

		static int counter;
		public static int Next() { return ++counter; }

		public static int Over(int a) { return 1000 + a; }
		public static int Over(long a) { return 2000 + (int)a; }

		public static string Greet(string name) { return "Hello, " + name + "!"; }
		public static int Len(string s) { return s.Length; }
		public static int CodeSum(string s) { int t = 0; for (int i = 0; i < s.Length; i++) t += s[i]; return t; }
		public static string Repeat(string s, int n) { string r = ""; for (int i = 0; i < n; i++) r += s; return r; }
		public static string Nothing() { return null; }
		public static string Join3(string a, string b, string c) { return a + "|" + b + "|" + c; }

		public static int Sum(int[] a) { int t = 0; for (int i = 0; i < a.Length; i++) t += a[i]; return t; }
		public static long SumL(long[] a) { long t = 0; for (int i = 0; i < a.Length; i++) t += a[i]; return t; }
		public static void Fill(int[] a, int v) { for (int i = 0; i < a.Length; i++) a[i] = v + i; }
		public static double Dot(double[] a, double[] b) { double t = 0; for (int i = 0; i < a.Length; i++) t += a[i] * b[i]; return t; }
		public static void Upper(byte[] a) { for (int i = 0; i < a.Length; i++) if (a[i] >= 97 && a[i] <= 122) a[i] = (byte)(a[i] - 32); }
		public static byte[] Bytes(int n) { byte[] r = new byte[n]; for (int i = 0; i < n; i++) r[i] = (byte)(i * 3); return r; }
		public static int[] Squares(int n) { int[] r = new int[n]; for (int i = 0; i < n; i++) r[i] = i * i; return r; }
		public static float[] Halves(int n) { float[] r = new float[n]; for (int i = 0; i < n; i++) r[i] = i * 0.5f; return r; }
		public static int[] NoInts() { return null; }
		public static int Count(string[] a) { return a.Length; }

		// What was passed in must survive a collection that happens while the managed code runs.
		public static int Churn(string s, int[] a, int rounds) {
			int sink = 0;
			for (int i = 0; i < rounds; i++) {
				int[] garbage = new int[200];
				garbage[0] = i;
				string junk = "x" + i;
				sink += garbage[0] + junk.Length;
			}
			return s.Length * 1000 + Sum(a) + (sink & 1);
		}
	}
}
