// (Floats are printed through (double) where they can be large or tiny: Single.ToString is wrong for those on wasm32, a bug of its own that
// has nothing to do with the JIT; see tests/bench/README or the repro in the commit message.)
// Semantics of the methods that the wasm JIT compiles (native/src/WasmJIT.c): each kernel below is a method with no calls, which is the
// subset it takes. The output must equal Mono's; run on the native runtime this checks the interpreter, on build/dna-wasm the compiled code.
using System;

class WasmJit {
    static int Cmp(double a, double b) {
        int r = 0;
        if (a < b) r |= 1; if (a <= b) r |= 2; if (a > b) r |= 4; if (a >= b) r |= 8; if (a == b) r |= 16; if (a != b) r |= 32;
        if (!(a < b)) r |= 64; if (!(a >= b)) r |= 128;
        bool lt = a < b, gt = a > b, eq = a == b, le = a <= b, ge = a >= b;
        if (lt) r |= 256; if (gt) r |= 512; if (eq) r |= 1024; if (le) r |= 2048; if (ge) r |= 4096;
        return r;
    }
    static int CmpF(float a, float b) {
        int r = 0;
        if (a < b) r |= 1; if (a <= b) r |= 2; if (a > b) r |= 4; if (a >= b) r |= 8; if (a == b) r |= 16; if (a != b) r |= 32;
        if (!(a < b)) r |= 64; if (!(a >= b)) r |= 128;
        bool lt = a < b, gt = a > b, ge = a >= b;
        if (lt) r |= 256; if (gt) r |= 512; if (ge) r |= 4096;
        return r;
    }
    static int CmpI(int a, int b) {
        int r = 0;
        if (a < b) r |= 1; if (a <= b) r |= 2; if (a > b) r |= 4; if (a >= b) r |= 8; if (a == b) r |= 16; if (a != b) r |= 32;
        bool lt = a < b, gt = a > b; if (lt) r |= 256; if (gt) r |= 512;
        return r;
    }
    static int CmpU(uint a, uint b) {
        int r = 0;
        if (a < b) r |= 1; if (a <= b) r |= 2; if (a > b) r |= 4; if (a >= b) r |= 8; if (a == b) r |= 16; if (a != b) r |= 32;
        bool lt = a < b, gt = a > b; if (lt) r |= 256; if (gt) r |= 512;
        return r;
    }
    static int CmpL(long a, long b) {
        int r = 0;
        if (a < b) r |= 1; if (a <= b) r |= 2; if (a > b) r |= 4; if (a >= b) r |= 8; if (a == b) r |= 16; if (a != b) r |= 32;
        bool lt = a < b, gt = a > b; if (lt) r |= 256; if (gt) r |= 512;
        return r;
    }
    static int CmpUL(ulong a, ulong b) {
        int r = 0;
        if (a < b) r |= 1; if (a <= b) r |= 2; if (a > b) r |= 4; if (a >= b) r |= 8; if (a == b) r |= 16; if (a != b) r |= 32;
        bool lt = a < b, gt = a > b; if (lt) r |= 256; if (gt) r |= 512;
        return r;
    }

    static int IntOps(int a, int b) {
        int r = 17;
        r = r * 31 + (a + b); r = r * 31 + (a - b); r = r * 31 + (a * b);
        r = r * 31 + (a & b); r = r * 31 + (a | b); r = r * 31 + (a ^ b);
        r = r * 31 + (a << (b & 31)); r = r * 31 + (a >> (b & 31)); r = r * 31 + (int)((uint)a >> (b & 31));
        r = r * 31 + (-a); r = r * 31 + (~a);
        return r;
    }
    static long LongOps(long a, long b) {
        long r = 17;
        r = r * 31 + (a + b); r = r * 31 + (a - b); r = r * 31 + (a * b);
        r = r * 31 + (a & b); r = r * 31 + (a | b); r = r * 31 + (a ^ b);
        r = r * 31 + (a << (int)(b & 63)); r = r * 31 + (a >> (int)(b & 63)); r = r * 31 + (long)((ulong)a >> (int)(b & 63));
        r = r * 31 + (-a); r = r * 31 + (~a);
        return r;
    }

    // conversions, including the values that have no result in the target type
    static int F2I(float f) { return (int)f; }
    static int D2I(double d) { return (int)d; }
    static long F2L(float f) { return (long)f; }
    static long D2L(double d) { return (long)d; }
    static int NarrowU8(int x) { return (byte)x; }
    static int NarrowI8(int x) { return (sbyte)x; }
    static int NarrowI16(int x) { return (short)x; }
    static int NarrowU16(int x) { return (ushort)x; }
    static int NarrowChar(int x) { return (char)x; }
    static long ToLong(int x) { return x; }
    static long ToULong(int x) { return (long)(uint)x; }
    static int FromLong(long x) { return (int)x; }
    static float I2F(int x) { return x; }
    static float L2F(long x) { return x; }
    static double I2D(int x) { return x; }
    static double L2D(long x) { return x; }
    static double F2D(float x) { return x; }
    static float D2F(double x) { return (float)x; }

    // small types in locals: every store truncates
    static int NarrowLocals(int n) {
        byte b = 0; sbyte sb = 0; short s = 0; ushort us = 0; char c = (char)0; bool z = false;
        int acc = 0;
        for (int i = 0; i < n; i++) {
            b += 37; sb += 37; s += 12345; us += 12345; c = (char)(c + 4097); z = !z;
            acc = acc * 7 + b + sb + s + us + c + (z ? 1 : 0);
        }
        return acc;
    }

    // a value on the evaluation stack across a branch (?: and && ||), nested loops, do/while
    static int Ternary(int x, int y) { return (x > 0 ? x * 2 : y - 1) + (x > y && y > 0 ? 100 : (x < -5 || y < -5 ? 200 : 300)); }
    static int Loops(int n) {
        int total = 0;
        for (int i = 0; i < n; i++)
            for (int j = i; j < n; j++) { if (((i ^ j) & 1) == 0) continue; total += i * j; if (total > 1000000) break; }
        int k = 0; do { total += k; k += 3; } while (k < n);
        while (total % 7 != 0) total++;
        return total;
    }
    static long Collatz(long n) { long steps = 0; while (n != 1) { n = (n & 1) == 0 ? n / 2 : 3 * n + 1; steps++; } return steps; }
    static int ManyArgs(int a, int b, int c, int d, int e, int f, int g, long h, float i, double j) {
        return a + b * 2 + c * 3 + d * 4 + e * 5 + f * 6 + g * 7 + (int)h + (int)i + (int)j;
    }

    // arrays: loads and stores of 4- and 8-byte elements, the length
    static float DotF(float[] a, float[] b, int n) { float s = 0f; for (int i = 0; i < n; i++) s += a[i] * b[i]; return s; }
    static double SumD(double[] a) { double s = 0; for (int i = 0; i < a.Length; i++) s += a[i]; return s; }
    static int SumI(int[] a, int n) { int s = 0; for (int i = 0; i < n; i++) s += a[i]; return s; }
    static void FillI(int[] a, int v) { for (int i = 0; i < a.Length; i++) a[i] = v + i; }
    static void ScaleD(double[] a, double k) { for (int i = 0; i < a.Length; i++) a[i] = a[i] * k; }
    static void AxpyF(float[] y, float[] x, float k, int n) { for (int i = 0; i < n; i++) y[i] += k * x[i]; }
    static int Prefix(int[] a) { int s = 0; for (int i = 1; i < a.Length; i++) { a[i] += a[i - 1]; s ^= a[i]; } return s; }
    static int ReadAt(int[] a, int i) { int x = a[i]; return x + 1; }
    static void WriteAt(int[] a, int i, int v) { a[i] = v; a[0] = a[0] + 1; }
    static int NullLen(int[] a) { return a.Length + 1; }

    // integer division: every variant, with the exceptions it raises
    static int DivI(int a, int b) { return a / b; }
    static int RemI(int a, int b) { return a % b; }
    static uint DivU(uint a, uint b) { return a / b; }
    static uint RemU(uint a, uint b) { return a % b; }
    static long DivL(long a, long b) { return a / b; }
    static long RemL(long a, long b) { return a % b; }
    static ulong DivUL(ulong a, ulong b) { return a / b; }
    static ulong RemUL(ulong a, ulong b) { return a % b; }
    static string DivRem(int op, long a, long b) {
        try {
            switch (op) {
                case 0: return DivI((int)a, (int)b).ToString();
                case 1: return RemI((int)a, (int)b).ToString();
                case 2: return DivU((uint)a, (uint)b).ToString();
                case 3: return RemU((uint)a, (uint)b).ToString();
                case 4: return DivL(a, b).ToString();
                case 5: return RemL(a, b).ToString();
                case 6: return DivUL((ulong)a, (ulong)b).ToString();
                default: return RemUL((ulong)a, (ulong)b).ToString();
            }
        } catch (DivideByZeroException) { return "DIV0"; } catch (OverflowException) { return "OVF"; }
    }

    // compound assignment on array elements (ldelema, then ldind / stind)
    static void BumpI(int[] a) { for (int i = 0; i < a.Length; i++) { a[i] += i; a[i] *= 2; a[i]++; a[i] -= 3; } }
    static void BumpF(float[] a) { for (int i = 0; i < a.Length; i++) { a[i] += 0.5f; a[i] *= 1.5f; a[i] -= 0.25f; } }
    static void BumpD(double[] a) { for (int i = 0; i < a.Length; i++) { a[i] += 0.5; a[i] /= 3.0; } }
    static void BumpAt(int[] a, int i) { a[i] += 5; }
    static void BumpFAt(float[] a, int i) { a[i] *= 2f; }

    // the Math / MathF methods that become one wasm instruction
    static double MathD(double x, double y) { return Math.Sqrt(Math.Abs(x)) + Math.Min(x, y) * 3 + Math.Max(x, y) * 5 + Math.Floor(x) + Math.Ceiling(y) * 7 + Math.Truncate(x - y); }
    static float MathS(float x, float y) { return MathF.Sqrt(MathF.Abs(x)) + MathF.Min(x, y) * 3 + MathF.Max(x, y) * 5 + MathF.Floor(x) + MathF.Ceiling(y) * 7 + MathF.Truncate(x - y); }
    static double MinMaxD(double x, double y) { return Math.Min(x, y) * 1000 + Math.Max(x, y); }
    static float MaxS(float x, float y) { return Math.Max(x, y); }
    static float MinS(float x, float y) { return Math.Min(x, y); }
    static double MaxD(double x, double y) { return Math.Max(x, y); }
    static double MinD(double x, double y) { return Math.Min(x, y); }
    static float MinMaxS(float x, float y) { return Math.Min(x, y) * 1000 + Math.Max(x, y); }
    static long MinMaxI(int a, int b, uint c, uint d, long e, long f, ulong g, ulong h) {
        return Math.Min(a, b) * 7L + Math.Max(a, b) * 11L + (long)Math.Min(c, d) * 13 + (long)Math.Max(c, d) * 17 + Math.Min(e, f) * 19 + Math.Max(e, f) * 23 + (long)(Math.Min(g, h) % 1000003) * 29 + (long)(Math.Max(g, h) % 1000003) * 31;
    }

    static string Try(Func<int> f) { try { return f().ToString(); } catch (IndexOutOfRangeException) { return "IOOR"; } catch (NullReferenceException) { return "NRE"; } }

    static void Main() {
        double[] ds = { 0.0, -0.0, 1.0, -1.0, 0.5, double.NaN, double.PositiveInfinity, double.NegativeInfinity, 1e300, -1e300 };
        float[] fs = { 0f, 1f, -1f, 0.5f, float.NaN, float.PositiveInfinity, float.NegativeInfinity, 3e38f };
        int[] ints = { 0, 1, -1, 2, 31, 32, 33, int.MaxValue, int.MinValue, 12345, -12345, 0x7ff0, 255, 256, 65535, 65536 };
        long[] longs = { 0, 1, -1, 63, 64, 65, long.MaxValue, long.MinValue, 123456789012345L, -123456789012345L };

        foreach (double a in ds) foreach (double b in ds) Console.WriteLine("cmp " + Cmp(a, b));
        foreach (float a in fs) foreach (float b in fs) Console.WriteLine("cmpf " + CmpF(a, b));
        foreach (int a in ints) foreach (int b in ints) Console.WriteLine("cmpi " + CmpI(a, b) + " " + CmpU((uint)a, (uint)b));
        foreach (long a in longs) foreach (long b in longs) Console.WriteLine("cmpl " + CmpL(a, b) + " " + CmpUL((ulong)a, (ulong)b));
        foreach (int a in ints) foreach (int b in ints) Console.WriteLine("int " + IntOps(a, b));
        foreach (long a in longs) foreach (long b in longs) Console.WriteLine("long " + LongOps(a, b));

        float[] fconv = { 0f, 1.5f, -1.5f, 0.99999f, -0.99999f, 2147483520f, 2147483648f, -2147483648f, -2147483904f, 4e9f, -4e9f, 1e20f, -1e20f, float.NaN, float.PositiveInfinity, float.NegativeInfinity, 9.3e18f, -9.3e18f, 9.2e18f };
        foreach (float f in fconv) Console.WriteLine("f2 " + F2I(f) + " " + F2L(f) + " " + D2I((double)f) + " " + D2L((double)f));
        double[] dconv = { 2147483647.9, 2147483648.0, -2147483648.9, -2147483649.0, 4294967296.0, 1e19, -1e19, 9223372036854775807.0, 123456789.987 };
        foreach (double d in dconv) Console.WriteLine("d2 " + D2I(d) + " " + D2L(d));
        foreach (int x in ints) Console.WriteLine("narrow " + NarrowU8(x) + " " + NarrowI8(x) + " " + NarrowI16(x) + " " + NarrowU16(x) + " " + NarrowChar(x) + " " + ToLong(x) + " " + ToULong(x));
        foreach (long x in longs) Console.WriteLine("fromlong " + FromLong(x) + " " + L2F(x) + " " + L2D(x));
        foreach (int x in ints) Console.WriteLine("int2f " + I2F(x) + " " + I2D(x));
        foreach (double x in ds) Console.WriteLine("d2f " + D2F(x) + " " + F2D(D2F(x)));

        foreach (double x in ds) foreach (double y in ds) Console.WriteLine("mathd " + MathD(x, y) + " " + MinMaxD(x, y));
        foreach (float x in fs) foreach (float y in fs) Console.WriteLine("maths " + (double)MathS(x, y) + " " + (double)MinMaxS(x, y));
        // (printed directly, so that a NaN cannot be hidden by the arithmetic around it)
        foreach (double x in ds) foreach (double y in ds) Console.WriteLine("mmd " + MinD(x, y) + " " + MaxD(x, y) + " " + Math.Min(x, y) + " " + Math.Max(x, y));
        foreach (float x in fs) foreach (float y in fs) Console.WriteLine("mms " + (double)MinS(x, y) + " " + (double)MaxS(x, y) + " " + (double)Math.Min(x, y) + " " + (double)Math.Max(x, y));
        foreach (int x in ints) foreach (int y in ints) Console.WriteLine("minmax " + MinMaxI(x, y, (uint)x, (uint)y, x * 3000000000L, y * 7L, (ulong)x * 5, (ulong)y));
        foreach (long x in longs) foreach (long y in longs) Console.WriteLine("minmaxl " + MinMaxI((int)x, (int)y, (uint)x, (uint)y, x, y, (ulong)x, (ulong)y));
        Console.WriteLine("narrowlocals " + NarrowLocals(1000));
        for (int x = -8; x < 8; x++) for (int y = -8; y < 8; y += 3) Console.WriteLine("ternary " + Ternary(x, y));
        Console.WriteLine("loops " + Loops(50) + " " + Loops(3) + " " + Loops(0));
        Console.WriteLine("collatz " + Collatz(27) + " " + Collatz(97) + " " + Collatz(871));
        Console.WriteLine("manyargs " + ManyArgs(1, 2, 3, 4, 5, 6, 7, 8L, 9.5f, 10.5));

        float[] fa = new float[50], fb = new float[50];
        for (int i = 0; i < 50; i++) { fa[i] = i * 0.25f; fb[i] = 3f - i * 0.1f; }
        Console.WriteLine("dotf " + DotF(fa, fb, 50));
        AxpyF(fa, fb, 0.5f, 50); Console.WriteLine("axpy " + DotF(fa, fa, 50));
        double[] da = new double[20]; for (int i = 0; i < 20; i++) da[i] = i * 1.5;
        ScaleD(da, 0.1); Console.WriteLine("scaled " + SumD(da));
        int[] arr = new int[30]; FillI(arr, 5); Console.WriteLine("fill " + SumI(arr, 30) + " prefix " + Prefix(arr) + " " + arr[29]);

        Console.WriteLine("ioor " + Try(() => ReadAt(arr, 30)) + " " + Try(() => ReadAt(arr, -1)) + " " + Try(() => ReadAt(arr, 29)) + " " + Try(() => ReadAt(null, 0)));
        Console.WriteLine("write " + Try(() => { WriteAt(arr, 31, 1); return 0; }) + " " + Try(() => { WriteAt(null, 0, 1); return 0; }) + " " + Try(() => { WriteAt(arr, 3, 77); return arr[3] + arr[0]; }));
        Console.WriteLine("nullen " + Try(() => NullLen(null)) + " " + Try(() => NullLen(arr)) + " " + Try(() => SumI(arr, 31)));
        // integer division and remainder: every pair of edge values, all eight variants
        foreach (long x in longs) foreach (long y in longs) {
            string line = "divrem";
            for (int op = 0; op < 8; op++) line += " " + DivRem(op, x, y);
            Console.WriteLine(line);
        }
        foreach (int x in ints) foreach (int y in ints) {
            string line = "divrem32";
            for (int op = 0; op < 4; op++) line += " " + DivRem(op, x, y);
            Console.WriteLine(line);
        }
        int[] ba = new int[10]; for (int i = 0; i < 10; i++) ba[i] = i * i; BumpI(ba); Console.WriteLine("bumpi " + SumI(ba, 10) + " " + ba[9]);
        float[] bf = new float[10]; for (int i = 0; i < 10; i++) bf[i] = i * 0.3f; BumpF(bf); Console.WriteLine("bumpf " + DotF(bf, bf, 10));
        double[] bd = new double[10]; for (int i = 0; i < 10; i++) bd[i] = i * 0.7; BumpD(bd); Console.WriteLine("bumpd " + SumD(bd));
        Console.WriteLine("bumpat " + Try(() => { BumpAt(ba, 10); return 0; }) + " " + Try(() => { BumpAt(null, 0); return 0; }) + " " + Try(() => { BumpAt(ba, -1); return 0; }) + " " + Try(() => { BumpAt(ba, 2); return ba[2]; }) + " " + Try(() => { BumpFAt(bf, 10); return 0; }));
        // the exception left the arrays as the code before it had made them
        Console.WriteLine("after " + arr[0] + " " + arr[3]);
    }
}
