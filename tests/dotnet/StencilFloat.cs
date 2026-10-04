using System;

// float32 compare-and-branch and int <-> float conversions in native blocks (see StencilBlocks.cs). All ten float branches
// (the ordered ones are false for NaN, the unordered .un ones true), in straight-line code and in loops; conv.r4, conv.i4 and the
// narrowing conv.i1 / conv.i2 of a float, including NaN, infinities and out-of-range values. Compared with Mono line for
// line; the suite also runs it with DNA_NO_STENCILS=1.
class Program {
    static uint h;
    static void Mix(uint v) { h = (h ^ v) * 16777619u; }
    static unsafe void MixF(float f) { if (f != f) { Mix(0x7fc00000u); } else { Mix(*(uint*)&f); } }

    // every comparison, taken as an if with an else (C# compiles `a < b` to the unordered bge.un, `a >= b` to blt.un, and so on)
    static int Compares(float a, float b) {
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
    static float Select(float a, float b, float c) {
        float r;
        if (a < b) r = a * c; else r = b * c;
        if (r >= 100f) r = r - 100f;
        if (!(r > 1f)) r = r + 1f;
        if (r == 3f) r = r * 2f;
        return r;
    }
    static int Count(float lim, int n) {
        int c = 0; float x = 0.5f;
        for (int i = 0; i < n; i++) {
            x = x * 1.01f + 0.1f;
            if (x < lim) c++;
            if (x == lim) c += 100;
            if (x != x) c += 1000;
            if (!(x <= lim)) c += 10000;
        }
        return c;
    }
    static int ConvLoop(int n, float scale) {
        int s = 0;
        for (int i = 0; i < n; i++) { float f = (float)i * scale; s += (int)f; }
        return s;
    }
    static int Narrow(float f) { int a = (sbyte)f; int b = (short)f; return a + 1000 * b; }
    static float IntToFloat(int i) { float f = (float)i; float g = f * 0.5f; return g + f; }
    static int FloatToInt(float f) { int i = (int)f; int j = i + 1; return j * 2; }

    static readonly float[] vals = { 0f, -0f, 1f, -1f, 0.5f, 3.14159f, 1e20f, -1e20f, 1e-20f, float.NaN,
        float.PositiveInfinity, float.NegativeInfinity, 16777216f, 7f, -2.5f, 3e9f, -3e9f, 127.9f, -129.5f, 40000.5f };
    static readonly int[] ints = { 0, 1, -1, 7, 127, 128, -129, 32767, 65537, 16777217, int.MaxValue, int.MinValue, 123456789 };

    static void Group(string name) { Console.WriteLine(name + " " + h); h = 2166136261u; }

    public static void Main() {
        h = 2166136261u;
        for (int i = 0; i < vals.Length; i++) for (int j = 0; j < vals.Length; j++) { Mix((uint)Compares(vals[i], vals[j])); }
        Group("compares");
        for (int i = 0; i < vals.Length; i++) for (int j = 0; j < vals.Length; j++) MixF(Select(vals[i], vals[j], vals[(i + j) % vals.Length]));
        Group("select");
        for (int i = 0; i < vals.Length; i++) { Mix((uint)Count(vals[i], 30)); Mix((uint)Count(vals[i], 3)); }
        Group("loops");
        for (int i = 0; i < vals.Length; i++) { Mix((uint)ConvLoop(40, vals[i])); Mix((uint)Narrow(vals[i])); Mix((uint)FloatToInt(vals[i])); }
        Group("float to int");
        foreach (int i in ints) MixF(IntToFloat(i));
        Group("int to float");
    }
}
