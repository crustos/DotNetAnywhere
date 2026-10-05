using System;
using System.Runtime.InteropServices;

// [DllImport] with more than six arguments (tests/ffi/mylib.json): past the six integer and eight floating-point argument registers a C call has the
// rest on the stack, which the call stencils cannot make, so these go through the wrapper. Integers past the sixth, doubles and floats past the eighth,
// a mix of them, and a string, an array, and ref arguments among them. Compared with Mono (same C as a shared library).
class Program {
    [DllImport("mylib")] static extern int sum9(int a, int b, int c, int d, int e, int f, int g, int h, int i);
    [DllImport("mylib")] static extern long sum8l(long a, long b, long c, long d, long e, long f, long g, long h);
    [DllImport("mylib")] static extern double mix12(int a, double b, long c, float d, int e, double f, int g, long h, float i, int j, double k, int l);
    [DllImport("mylib")] static extern double sum10d(double a, double b, double c, double d, double e, double f, double g, double h, double i, double j);
    [DllImport("mylib")] static extern float sum10f(float a, float b, float c, float d, float e, float f, float g, float h, float i, float j);
    [DllImport("mylib")] static extern int wide_str(string s, int a, int b, int c, int d, int e, int f, int g);
    [DllImport("mylib")] static extern int wide_buf(int[] p, int n, int a, int b, int c, int d, int e, int f, int g);
    [DllImport("mylib")] static extern void wide_out(int a, int b, int c, int d, int e, int f, int g, out int o1, out int o2);

    static uint h;
    static void Mix(uint v) { h = (h ^ v) * 16777619u; }
    static void MixL(long v) { Mix((uint)v); Mix((uint)(v >> 32)); }
    static void Group(string name) { Console.WriteLine(name + " " + h); h = 2166136261u; }

    public static void Main() {
        h = 2166136261u;
        for (int k = -3; k <= 40; k += 3) Mix((uint)sum9(k, k + 1, k * 2, 7 - k, k ^ 5, k * k, 100 - k, k + 9, -k));
        Group("sum9 (nine ints)");
        for (long k = -3; k <= 40; k += 5) MixL(sum8l(k << 33, k, -k, k * 77, 1L << 40, k + 5, -(k << 20), k * k));
        Group("sum8l (eight longs)");
        for (int k = 0; k < 30; k++)
            MixL((long)(mix12(k, k * 0.5, (long)k << 35, k * 0.25f, k + 1, -k * 1.5, 3 - k, (long)-k * 99, k / 3f, k * 2, k * 0.125, k ^ 3) * 1000));
        Group("mix12 (ints, longs, floats and doubles)");
        for (int k = 0; k < 25; k++) MixL((long)(sum10d(k, k * 0.5, k - 3.5, k * k * 0.01, -k, 1.25, k * 7.5, 0.5 - k, k / 7.0, 100 - k) * 1000));
        Group("sum10d (ten doubles)");
        for (int k = 0; k < 25; k++) MixL((long)(sum10f(k, k * 0.5f, k - 3.5f, k * k * 0.01f, -k, 1.25f, k * 7.5f, 0.5f - k, k / 7f, 100 - k) * 100));
        Group("sum10f (ten floats)");
        string[] ss = { "", "a", "h\u00e9llo", "\u65e5\u672c\u8a9e", "smile \U0001F600 done" };
        for (int k = 0; k < 15; k++) Mix((uint)wide_str(ss[k % 5], k, k + 1, k * 3, -k, 9, k ^ 6, k * k));
        Group("wide_str (a string and seven ints)");
        int[] a = new int[20];
        for (int i = 0; i < a.Length; i++) a[i] = i * i - 5 * i;
        for (int n = 0; n <= 20; n += 4) Mix((uint)wide_buf(a, n, n, 2, 3, 4, 5, 6, n + 1));
        Group("wide_buf (an array and eight ints)");
        for (int k = -2; k < 12; k++) {
            int o1, o2;
            wide_out(k, k + 1, k + 2, 3 - k, k * 2, 5, k ^ 9, out o1, out o2);
            Mix((uint)o1); Mix((uint)o2);
        }
        Group("wide_out (seven ints and two out)");
    }
}
