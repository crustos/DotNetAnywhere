using System;
using System.Runtime.InteropServices;

// [DllImport] of C functions named in an FFI manifest (tests/ffi/mylib.json: build.py --ffi) is a direct call. Every function of the manifest,
// with values that show how arguments and results are passed: sign and zero extension of narrow results, 64-bit integers, integer and
// floating-point arguments mixed (they go in different registers), six integer arguments, a pointer, a function with state, and calls made
// where the evaluation stack is not empty, in loops, and through a delegate. Compared with Mono, which runs the same C as a shared library.
class Program {
    [DllImport("mylib", EntryPoint = "add_numbers", CallingConvention = CallingConvention.Cdecl)] public static extern int AddNumbers(int a, int b);
    [DllImport("mylib")] public static extern int sum6(int a, int b, int c, int d, int e, int f);
    [DllImport("mylib")] public static extern long mix64(long a, int b);
    [DllImport("mylib")] public static extern uint uclamp(uint v, uint lo, uint hi);
    [DllImport("mylib")] public static extern double hyp2(double a, double b);
    [DllImport("mylib")] public static extern float scalef(float x, float k);
    [DllImport("mylib")] public static extern double mixed(int a, double b, long c, float d, int e);
    [DllImport("mylib")] public static extern short narrow16(int v);
    [DllImport("mylib")] public static extern byte low8(int v);
    [DllImport("mylib")] public static extern IntPtr ptr_bump(IntPtr p, int n);
    [DllImport("mylib")] public static extern void bump_counter(int by);
    [DllImport("mylib")] public static extern int get_counter();
    [DllImport("mylib")] public static extern int bump_get(int by);
    [DllImport("mylib")] public static extern double aligned_sum(double a, double b);

    static uint h;
    static void Mix(uint v) { h = (h ^ v) * 16777619u; }
    static void MixL(long v) { Mix((uint)v); Mix((uint)(v >> 32)); }
    static void MixD(double d) { MixL((long)(d * 1000.0)); }
    static void Group(string name) { Console.WriteLine(name + " " + h); h = 2166136261u; }

    delegate int Bin(int a, int b);
    static int Plain(int a, int b) { return a * 3 - b; }
    static int ViaWrapper(int a, int b) { return AddNumbers(a, b) + AddNumbers(b, a) * 2; }

    public static void Main() {
        h = 2166136261u;
        int[] vals = { 0, 1, -1, 2, 127, 128, 255, 256, -129, 32767, 32768, 65535, 65536, 123456789, int.MaxValue, int.MinValue };
        foreach (int a in vals) foreach (int b in vals) { Mix((uint)AddNumbers(a, b)); MixL(mix64((long)a << 20 | (uint)b, b)); }
        Group("int and long");
        foreach (int a in vals) { Mix((uint)narrow16(a)); Mix((uint)low8(a)); Mix((uint)(narrow16(a) + low8(a))); Mix((uint)uclamp((uint)a, 100u, 70000u)); }
        Group("narrow results and unsigned");
        for (int i = -5; i < 25; i++) { Mix((uint)sum6(i, i + 1, i * 2, -i, 7, i ^ 5)); MixD(hyp2(i * 0.5, 1.5 - i)); MixD(scalef(i * 0.25f, 3f - i)); }
        Group("six arguments, double, float");
        for (int i = -4; i < 20; i++) { MixD(mixed(i, i * 0.5, (long)i << 33, i / 3f, 7 - i)); }
        Group("integer and floating point arguments mixed");
        long p0 = 1000000007L;
        for (int i = 0; i < 20; i++) { IntPtr r = ptr_bump(new IntPtr(p0 + i), i * 5); MixL(r.ToInt64()); }
        Group("pointer");
        bump_counter(5); Mix((uint)get_counter());
        for (int i = 0; i < 50; i++) { bump_counter(i); Mix((uint)bump_get(i * 2 + 1)); Mix((uint)get_counter()); }
        Group("state in C");
        // calls with other things on the evaluation stack, results used in expressions, as arguments to each other, through a delegate
        for (int i = 0; i < 40; i++) {
            int x = 1000 + i * 7 + AddNumbers(i, AddNumbers(i, 3)) * 2 - sum6(1, 2, 3, 4, 5, i) + (int)hyp2(i, 2.0);
            Mix((uint)x); Mix((uint)ViaWrapper(i, 10 - i));
        }
        Bin d = AddNumbers; Bin d2 = Plain; Bin d3 = ViaWrapper;
        for (int i = 0; i < 30; i++) { Mix((uint)d(i, i * 2)); Mix((uint)d2(i, 3)); Mix((uint)d3(i, 4)); }
        Group("in expressions and through delegates");
        // a C function that needs the stack aligned, called from native blocks (loops) and from the interpreter, with other things on the stack
        for (int i = 0; i < 30; i++) { MixD(aligned_sum(i * 0.5, 3.0 - i) + (double)AddNumbers(i, 1) + aligned_sum(i, i + 1)); }
        double ad = 0; for (int i = 0; i < 2000; i++) { ad += aligned_sum(i, 0.25); } MixD(ad);
        Group("stack alignment");
        long acc = 0;
        for (int i = 0; i < 20000; i++) { acc += AddNumbers(i & 1023, 5); acc ^= sum6(i, 1, 2, 3, 4, 5) & 255; }
        MixL(acc);
        Group("a loop");
    }
}
