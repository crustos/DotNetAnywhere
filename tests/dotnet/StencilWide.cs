using System;

// long and double in native blocks (see StencilBlocks.cs): 64-bit integer arithmetic, shifts, comparisons and loops; double
// arithmetic, comparisons (the ordered ones false for NaN, the unordered true) and loops; 64-bit constants (two stencils: the
// low half and the high half); and every conversion that changes the size of the value on the stack. Compared with Mono line for
// line; the suite also runs it with DNA_NO_STENCILS=1.
class Program {
    static uint h;
    static void Mix(uint v) { h = (h ^ v) * 16777619u; }
    static void MixL(long v) { Mix((uint)v); Mix((uint)(v >> 32)); }
    static unsafe void MixD(double d) { if (d != d) { Mix(0x7ff80000u); Mix(0); } else { long b = *(long*)&d; MixL(b); } }
    static unsafe void MixF(float f) { if (f != f) { Mix(0x7fc00000u); } else { Mix(*(uint*)&f); } }

    static long LongOps(long a, long b, int n) {
        long r = a + b; r = r * 3 - b; r = r ^ (a << (n & 63)); r = r | (b >> (n & 63)); r = r & ~(a >> 3);
        r = -r + (long)n;
        return r;
    }
    static ulong UlongOps(ulong a, ulong b, int n) { ulong r = a * b + (a >> (n & 63)); r = r ^ (b << 7); r = r | (a >> 33); return r; }
    static long Consts() { long a = 0x123456789ABCDEF0L; long b = -1L; long c = long.MinValue; long d = 0x00000001FFFFFFFFL; return a + b * 3 + (c >> 7) - d; }
    static double DConsts() { double a = 3.141592653589793; double b = 1e300; double c = -0.0; double d = 1e-310; return a * 2.0 + b / 1e290 + c + d * 1e300; }
    static int Compares(long a, long b) {
        int r = 0;
        if (a < b) r += 1; else r += 2;
        if (a <= b) r += 4; else r += 8;
        if (a > b) r += 16; else r += 32;
        if (a >= b) r += 64; else r += 128;
        if (a == b) r += 256; else r += 512;
        if (a != b) r += 1024; else r += 2048;
        return r;
    }
    static int DCompares(double a, double b) {
        int r = 0;
        if (a < b) r += 1; else r += 2;
        if (a <= b) r += 4; else r += 8;
        if (a > b) r += 16; else r += 32;
        if (a >= b) r += 64; else r += 128;
        if (a == b) r += 256; else r += 512;
        if (a != b) r += 1024; else r += 2048;
        if (!(a < b)) r += 4096; else r += 8192;
        if (!(a <= b)) r += 16384; else r += 32768;
        if (!(a > b)) r += 65536; else r += 131072;
        if (!(a >= b)) r += 262144; else r += 524288;
        return r;
    }
    static double DoubleOps(double a, double b, double c) {
        double r = a * b + c; r = r - a / (b + 1.5); r = -r * 0.5 + c; return r;
    }
    // loops that keep everything in 64-bit locals
    static long SumLoop(int n) { long s = 0; for (int i = 0; i < n; i++) { s += (long)i * i; s ^= (long)i << 7; } return s; }
    static long LongLoop(long n) { long s = 1; for (long j = 1; j <= n; j += 3) { s = s * 31 + j; if (s < 0) s = -s; } return s; }
    static double DoubleLoop(int n) {
        double x = 0.0, y = 1.0;
        for (int i = 1; i <= n; i++) { x += 1.0 / ((double)i * i); y = y * 0.999999 + x; if (y > 1000.0) y = y - 1000.0; }
        return x + y;
    }
    static int DoubleCount(double lim, int n) {
        int c = 0; double x = 0.5;
        for (int i = 0; i < n; i++) { x = x * 1.01 + 0.1; if (x < lim) c++; if (x == lim) c += 100; if (x != x) c += 1000; if (!(x <= lim)) c += 10000; }
        return c;
    }
    // conversions: int/uint to long, long to int (and narrower), int to double, double to int, float and double
    static long Widen(int i, uint u) { long a = i; long b = u; long c = (long)i * 3; return a + b + c; }
    static ulong WidenU(int i, uint u) { ulong a = u; ulong b = (ulong)(uint)i; ulong c = a * 3 + b; return c ^ (a << 5); }
    static double Negate(double a, double b) { double r = a - b; r = -r * 2.5; r = r - -a; return r + 0.125; }
    static int Narrow(long v) { int a = (int)v; int b = (short)v; int c = (sbyte)v; return a + b * 3 + c * 5; }
    static double IntToDouble(int i) { double d = i; double e = d * 0.5; return e + d; }
    static int DoubleToInt(double d) { int i = (int)d; int j = (short)d; int k = (sbyte)d; return i + j * 3 + k * 5; }
    static float DoubleToFloat(double d) { float f = (float)d; float g = f * 2f; return g + f; }
    static double FloatToDouble(float f) { double d = f; return d * 1.5 + d; }

    static readonly long[] longs = { 0L, 1L, -1L, 2L, 63L, 64L, 255L, -255L, int.MaxValue, int.MinValue, (long)int.MaxValue + 1, (long)int.MinValue - 1,
        long.MaxValue, long.MinValue, 0x123456789ABCDEF0L, -987654321987654321L };
    static readonly double[] dbls = { 0.0, -0.0, 1.0, -1.0, 0.5, 3.14159, 1e300, -1e300, 1e-300, double.NaN, double.PositiveInfinity,
        double.NegativeInfinity, 4503599627370496.0, 7.0, -2.5, 3e9, -3e9, 127.9, -129.5, 40000.5, 2147483648.0, -2147483649.0 };
    static readonly int[] ints = { 0, 1, -1, 7, 127, 128, -129, 32767, 65537, 16777217, int.MaxValue, int.MinValue, 123456789 };

    static void Group(string name) { Console.WriteLine(name + " " + h); h = 2166136261u; }

    public static void Main() {
        h = 2166136261u;
        for (int i = 0; i < longs.Length; i++) for (int j = 0; j < longs.Length; j++) {
            MixL(LongOps(longs[i], longs[j], i * 5 + j)); Mix((uint)Compares(longs[i], longs[j]));
            MixL((long)UlongOps((ulong)longs[i], (ulong)longs[j], i + j * 3));
        }
        MixL(Consts());
        Group("long");
        for (int i = 0; i < dbls.Length; i++) for (int j = 0; j < dbls.Length; j++) {
            Mix((uint)DCompares(dbls[i], dbls[j])); MixD(DoubleOps(dbls[i], dbls[j], dbls[(i + j) % dbls.Length]));
        }
        MixD(DConsts());
        Group("double");
        for (int n = 0; n < 30; n++) { MixL(SumLoop(n)); MixL(LongLoop(n)); MixD(DoubleLoop(n)); }
        MixL(SumLoop(30000)); MixL(LongLoop(50000)); MixD(DoubleLoop(30000));
        for (int i = 0; i < dbls.Length; i++) { Mix((uint)DoubleCount(dbls[i], 40)); Mix((uint)DoubleCount(dbls[i], 3)); }
        Group("loops");
        foreach (int i in ints) foreach (int j in ints) { MixL(Widen(i, (uint)j)); MixL((long)WidenU(i, (uint)j)); }
        foreach (double a in dbls) foreach (double b in dbls) MixD(Negate(a, b));
        foreach (long v in longs) Mix((uint)Narrow(v));
        foreach (int i in ints) MixD(IntToDouble(i));
        foreach (double d in dbls) { Mix((uint)DoubleToInt(d)); MixF(DoubleToFloat(d)); }
        foreach (double d in dbls) MixD(FloatToDouble((float)d));
        Group("conversions");
    }
}
