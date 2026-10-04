using System;
using System.Runtime.InteropServices;

// [DllImport] with arrays, ref and out arguments, and strings (tests/ffi/mylib.json): an array is passed as a pointer to its elements, null as null; ref and
// out as the managed pointer; a string as a temporary UTF-8 copy; a string result is made a string and freed. Compared with Mono (same C as a
// shared library). Strings: empty, ASCII, accents, CJK, a surrogate pair, one longer than the buffer on the stack, null.
class Program {
    [DllImport("mylib")] static extern int sum_buf(int[] p, int n);
    [DllImport("mylib")] static extern long sum_i64(long[] p, int n);
    [DllImport("mylib")] static extern void fill_buf(int[] p, int n, int v);
    [DllImport("mylib")] static extern double dot(double[] a, double[] b, int n);
    [DllImport("mylib")] static extern void upcase(byte[] b, int n);
    [DllImport("mylib")] static extern int is_null(int[] p);
    [DllImport("mylib")] static extern void swap_ref(ref int a, ref int b);
    [DllImport("mylib")] static extern void divmod(int a, int b, out int q, out int r);
    [DllImport("mylib")] static extern int str_len(string s);
    [DllImport("mylib")] static extern int str_sum(string s);
    [DllImport("mylib")] static extern string greeting(int k);
    [DllImport("mylib")] static extern string echo_upper(string s);

    static uint h;
    static void Mix(uint v) { h = (h ^ v) * 16777619u; }
    static void MixL(long v) { Mix((uint)v); Mix((uint)(v >> 32)); }
    static void MixS(string s) { if (s == null) { Mix(0xdeadu); return; } Mix((uint)s.Length); for (int i = 0; i < s.Length; i++) Mix((uint)s[i]); }
    static void Group(string name) { Console.WriteLine(name + " " + h); h = 2166136261u; }

    struct Pt { public int X, Y; }
    class Holder { public int a = 5, b = 9; }
    static int sf1 = 11, sf2 = 22;

    public static void Main() {
        h = 2166136261u;
        int[] a = new int[50]; long[] l = new long[30]; double[] x = new double[20], y = new double[20]; byte[] bytes = new byte[40];
        for (int i = 0; i < a.Length; i++) a[i] = i * i - 7 * i;
        for (int i = 0; i < l.Length; i++) l[i] = (long)i << 33 | (uint)(i * 12345);
        for (int i = 0; i < x.Length; i++) { x[i] = i * 0.5; y[i] = 3.0 - i; }
        for (int n = 0; n <= 50; n += 7) { Mix((uint)sum_buf(a, n)); }
        for (int n = 0; n <= 30; n += 6) MixL(sum_i64(l, n));
        for (int n = 0; n <= 20; n += 5) MixL((long)(dot(x, y, n) * 1000));
        Mix((uint)is_null(a)); Mix((uint)is_null(null));
        Group("arrays in");
        fill_buf(a, 50, 100); for (int i = 0; i < a.Length; i++) Mix((uint)a[i]);
        int[] small = new int[3]; fill_buf(small, 3, -5); foreach (int v in small) Mix((uint)v);
        string text = "the quick brown fox 0123"; for (int i = 0; i < text.Length; i++) bytes[i] = (byte)text[i];
        upcase(bytes, 40); for (int i = 0; i < 40; i++) Mix(bytes[i]);
        // a window of an array is not what is passed: the array's own first element is
        fill_buf(a, 0, 1); Mix((uint)a[0]);
        Group("arrays out");
        int p = 3, q = 8; swap_ref(ref p, ref q); Mix((uint)p); Mix((uint)q);
        int quo, rem; for (int i = -7; i <= 7; i += 3) for (int j = 1; j <= 4; j++) { divmod(i * 13, j, out quo, out rem); Mix((uint)quo); Mix((uint)rem); }
        int[] arr2 = { 1, 2, 3, 4 }; swap_ref(ref arr2[0], ref arr2[3]); foreach (int v in arr2) Mix((uint)v);
        Pt pt = new Pt { X = 10, Y = 20 }; swap_ref(ref pt.X, ref pt.Y); Mix((uint)pt.X); Mix((uint)pt.Y);
        Holder ho = new Holder(); swap_ref(ref ho.a, ref ho.b); Mix((uint)ho.a); Mix((uint)ho.b);
        swap_ref(ref sf1, ref sf2); Mix((uint)sf1); Mix((uint)sf2);
        for (int i = 0; i < 100; i++) { int u = i, v = i * 3; swap_ref(ref u, ref v); Mix((uint)(u - v)); }
        Group("ref and out");
        string[] strs = { "", "a", "hello", "h\u00e9llo w\u00f6rld", "\u65e5\u672c\u8a9e", "smile \ud83d\ude00 done", new string('x', 300), new string('\u00e9', 200), new string('y', 254), new string('y', 255), new string('y', 256), new string('\u00e9', 127), new string('\u00e9', 128), new string('\u65e5', 85), new string('\u65e5', 86), new string('\u65e5', 100), null, "tab\there" };
        foreach (string s in strs) { Mix(s == null ? 1u : 0u); if (s != null) { Mix((uint)str_len(s)); Mix((uint)str_sum(s)); } MixS(echo_upper(s)); }
        Group("strings in");
        for (int k = 0; k < 8; k++) MixS(greeting(k));
        MixS(greeting(-1)); Mix((uint)greeting(3).Length);
        Group("strings out");
        long acc = 0;
        for (int i = 0; i < 2000; i++) { acc += sum_buf(small, 3) + str_len("abc" + (i & 7)); acc ^= greeting(i).Length; }
        MixL(acc);
        Group("a loop");
    }
}
