using System;
using System.Runtime.InteropServices;

// [DllImport] with delegates as C function pointers (tests/ffi/mylib.json "callbacks"): C calls a delegate during the call it was passed to. A static method,
// a lambda that captures a local, an instance method (the target must stay alive), two delegates of one type at once, a null delegate, a comparator that
// is handed raw pointers, thousands of calls that allocate (so a collection runs while C is in the middle of the call), a delegate alongside more than six
// arguments, and a callback that itself calls into C with another callback. Compared with Mono (same C as a shared library).
class Program {
    delegate int Binop(int a, int b);
    delegate void Visit(int i, double v);
    delegate double MapD(double x);
    delegate long Acc(long acc, float x, IntPtr tag);
    delegate int Cmp(IntPtr a, IntPtr b);

    [DllImport("mylib")] static extern int fold_ints(int[] a, int n, int init, Binop f);
    [DllImport("mylib")] static extern double map_sum(double[] a, int n, MapD f);
    [DllImport("mylib")] static extern void for_each(int n, Visit f);
    [DllImport("mylib")] static extern long accum(int n, Acc f, IntPtr tag);
    [DllImport("mylib")] static extern void sort_ints(int[] a, int n, Cmp f);
    [DllImport("mylib")] static extern int call_twice(Binop a, Binop b, int x, int y);
    [DllImport("mylib")] static extern int call_null(Binop f);
    [DllImport("mylib")] static extern int many(int count, Binop f);
    [DllImport("mylib")] static extern double wide_cb(int a, int b, int c, int d, int e, double f, Binop fn, int g);

    static uint h;
    static void Mix(uint v) { h = (h ^ v) * 16777619u; }
    static void MixL(long v) { Mix((uint)v); Mix((uint)(v >> 32)); }
    static void MixD(double d) { MixL((long)(d * 1000)); }
    static void Group(string name) { Console.WriteLine(name + " " + h); h = 2166136261u; }

    static int Add(int a, int b) { return a + b; }
    static int Mul(int a, int b) { return a * 3 + b; }
    class Counter {
        public int Calls, Sum;
        public int Step(int a, int b) { Calls++; Sum += a ^ b; return a + b + Calls; }
    }
    static unsafe int Ascending(IntPtr a, IntPtr b) { return *(int*)a.ToInt64() - *(int*)b.ToInt64(); }
    static unsafe int Descending(IntPtr a, IntPtr b) { return *(int*)b.ToInt64() - *(int*)a.ToInt64(); }
    static int sink;

    public static void Main() {
        h = 2166136261u;
        int[] a = new int[30];
        for (int i = 0; i < a.Length; i++) a[i] = (i * 37) % 23 - 9;
        for (int n = 0; n <= 30; n += 6) Mix((uint)fold_ints(a, n, 5, Add));
        for (int n = 0; n <= 30; n += 6) Mix((uint)fold_ints(a, n, 1, new Binop(Mul)));
        int bias = 7;
        for (int n = 0; n <= 30; n += 10) Mix((uint)fold_ints(a, n, 0, delegate (int x, int y) { return x + y * bias - 1; }));
        Group("fold_ints (a static method, a lambda that captures)");
        Counter c = new Counter();
        Mix((uint)fold_ints(a, 30, 0, c.Step));
        Mix((uint)c.Calls); Mix((uint)c.Sum);
        Mix((uint)fold_ints(a, 12, 0, c.Step));
        Mix((uint)c.Calls); Mix((uint)c.Sum);
        Group("an instance method (the target)");
        double[] d = new double[16];
        for (int i = 0; i < d.Length; i++) d[i] = i * 0.25 - 1;
        double scale = 1.5;
        MixD(map_sum(d, 16, delegate (double x) { return x * x * scale; }));
        MixD(map_sum(d, 7, Math.Abs));
        Group("map_sum (doubles)");
        double total = 0; int count = 0;
        for_each(10, delegate (int i, double v) { total += v * (i + 1); count++; });
        MixD(total); Mix((uint)count);
        Group("for_each (a callback with no result)");
        for (int n = 0; n < 12; n++) MixL(accum(n, delegate (long acc, float x, IntPtr tag) { return acc * 3 + (long)(x * 4) + tag.ToInt64(); }, new IntPtr(n * 5)));
        Group("accum (a long, a float and a pointer)");
        int[] s = new int[25];
        for (int i = 0; i < s.Length; i++) s[i] = (i * 11) % 17 - 6;
        sort_ints(s, s.Length, Ascending);
        for (int i = 0; i < s.Length; i++) Mix((uint)s[i]);
        sort_ints(s, 20, Descending);
        for (int i = 0; i < s.Length; i++) Mix((uint)s[i]);
        Group("sort_ints (a comparator that reads through the pointers)");
        Mix((uint)call_twice(Add, Mul, 4, 9));
        Mix((uint)call_twice(delegate (int x, int y) { return x - y; }, delegate (int x, int y) { return x * y; }, 6, -5));
        Mix((uint)call_null(null)); Mix((uint)call_null(Add));
        Group("two callbacks of one type at once, and null");
        int sum = 0;
        for (int round = 0; round < 3; round++)
            sum += many(3000, delegate (int x, int y) { int[] junk = new int[16]; junk[x & 15] = y; string t = "n" + x; sink += t.Length; return junk[x & 15] + (x & 3); });
        Mix((uint)sum);
        Group("many calls that allocate (collections run during the call)");
        for (int k = 0; k < 6; k++) MixD(wide_cb(k, k + 1, 2, 3, k * 4, k * 0.5, Add, 100 - k));
        Group("wide_cb (a callback among eight arguments)");
        int[] inner = new int[6];
        for (int i = 0; i < inner.Length; i++) inner[i] = i + 1;
        int reentered = 0;
        Mix((uint)many(8, delegate (int x, int y) {
            reentered++;
            return fold_ints(inner, 6, x, delegate (int p, int q) { return p + q * (y + 1); });
        }));
        Mix((uint)reentered);
        Group("a callback that calls into C with a callback");
    }
}
