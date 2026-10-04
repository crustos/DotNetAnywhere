using System;

// Every pattern the fusion pass (tools/gen_fused_ops.py, FuseOps in JIT.c) turns into one instruction, over edge values,
// and the places it must NOT fuse: a branch target inside what would be a run (the first operand arrives by two
// paths), and the boundaries of try blocks. Compared with Mono line for line; the same program also runs with
// DNA_NO_FUSION=1 in the test suite, and the two must agree.
class Program {
    static uint h;
    static void Mix(uint v) { h = (h ^ v) * 16777619u; }
    static unsafe void MixF(float f) { if (f != f) { Mix(0x7fc00000u); } else { Mix(*(uint*)&f); } }

    // ---- local op local (arguments are locals too)
    static int AddLL(int a, int b) { return a + b; }
    static int SubLL(int a, int b) { return a - b; }
    static int MulLL(int a, int b) { return a * b; }
    static int AndLL(int a, int b) { return a & b; }
    static int OrLL(int a, int b) { return a | b; }
    static int XorLL(int a, int b) { return a ^ b; }
    static float FAddLL(float a, float b) { return a + b; }
    static float FSubLL(float a, float b) { return a - b; }
    static float FMulLL(float a, float b) { return a * b; }
    static float FDivLL(float a, float b) { return a / b; }

    // ---- local op constant
    static int AddLC(int a) { return a + 100000; }
    static int SubLC(int a) { return a - 7; }
    static int MulLC(int a) { return a * 3; }
    static int AndLC(int a) { return a & 0xff00; }
    static int OrLC(int a) { return a | 5; }
    static int XorLC(int a) { return a ^ -1; }
    static int ShlLC(int a) { return a << 3; }
    static int ShrLC(int a) { return a >> 31; }
    static int ShrLC2(int a) { return a >> 5; }
    static uint ShrUnLC(int a) { return (uint)a >> 5; }
    static float FAddLC(float a) { return a + 1.5f; }
    static float FSubLC(float a) { return a - 0.25f; }
    static float FMulLC(float a) { return a * 2.5f; }
    static float FDivLC(float a) { return a / 3f; }

    // ---- compare and branch, locals against locals and against constants (all six, both operand forms)
    static int BeqLL(int a, int b) { if (a == b) return 1; return 0; }
    static int BneLL(int a, int b) { if (a != b) return 1; return 0; }
    static int BltLL(int a, int b) { if (a < b) return 1; return 0; }
    static int BleLL(int a, int b) { if (a <= b) return 1; return 0; }
    static int BgtLL(int a, int b) { if (a > b) return 1; return 0; }
    static int BgeLL(int a, int b) { if (a >= b) return 1; return 0; }
    static int BeqLC(int a) { if (a == 7) return 1; return 0; }
    static int BneLC(int a) { if (a != 7) return 1; return 0; }
    static int BltLC(int a) { if (a < 0) return 1; return 0; }
    static int BleLC(int a) { if (a <= 1) return 1; return 0; }
    static int BgtLC(int a) { if (a > -1) return 1; return 0; }
    static int BgeLC(int a) { if (a >= 2) return 1; return 0; }

    // ---- in-place increment, and two loads in a row
    static int Counting(int n) { int s = 0; for (int i = 0; i < n; i++) { s += i; } return s; }
    static int CountingBy(int n) { int s = 0; for (int i = 0; i < n; i += 5) { s += i; } return s; }
    static int CountingDown(int n) { int s = 0; for (int i = n; i > 0; i -= 3) { s += i; } return s; }
    static int Max2(int a, int b) { return a > b ? a : b; }
    static int Pick(int a, int b) { return Max2(a, b) - Max2(b, a) + Max2(a, a); }

    // ---- where fusing would be WRONG: a branch target inside the would-be run
    static int TernaryLeft(bool f, int a, int b, int c) { return (f ? a : b) + c; }
    static int TernaryRight(bool f, int a, int b, int c) { return c + (f ? a : b); }
    static int TernaryCompare(bool f, int a, int b, int c) { if ((f ? a : b) < c) return 1; return 0; }
    static float TernaryFloat(bool f, float a, float b, float c) { return (f ? a : b) * c; }
    static int NestedTernary(int x, int a, int b, int c) { return (x > 0 ? (x > 5 ? a : b) : c) - a; }
    static int ShortCircuit(int a, int b, int c) { return ((a > 0 && b > 0) ? a : c) + b; }

    // ---- try blocks begin and end between instructions that could otherwise be fused
    static int InTry(int a, int b) {
        int r = 0;
        try { r = a + b; r = r * 2 + a; if (r < 0) throw new Exception("neg"); r = r - b; }
        catch (Exception) { r = a - b; }
        finally { r = r + 1; }
        return r + a;
    }

    static readonly int[] ints = { 0, 1, -1, 2, 7, 31, 32, -31, 255, 65535, int.MaxValue, int.MinValue, 123456789, -987654321 };
    static readonly float[] floats = { 0f, -0f, 1f, -1f, 0.5f, 3.14159f, 1e20f, -1e20f, 1e-20f, float.NaN,
        float.PositiveInfinity, float.NegativeInfinity, 16777216f, 7f, -2.5f };

    static void Group(string name) { Console.WriteLine(name + " " + h); h = 2166136261u; }

    public static void Main() {
        h = 2166136261u;
        foreach (int a in ints) foreach (int b in ints) {
            Mix((uint)AddLL(a, b)); Mix((uint)SubLL(a, b)); Mix((uint)MulLL(a, b)); Mix((uint)AndLL(a, b)); Mix((uint)OrLL(a, b)); Mix((uint)XorLL(a, b));
        }
        Group("int LL");
        foreach (int a in ints) {
            Mix((uint)AddLC(a)); Mix((uint)SubLC(a)); Mix((uint)MulLC(a)); Mix((uint)AndLC(a)); Mix((uint)OrLC(a)); Mix((uint)XorLC(a));
            Mix((uint)ShlLC(a)); Mix((uint)ShrLC(a)); Mix((uint)ShrLC2(a)); Mix(ShrUnLC(a));
        }
        Group("int LC");
        foreach (float a in floats) foreach (float b in floats) { MixF(FAddLL(a, b)); MixF(FSubLL(a, b)); MixF(FMulLL(a, b)); MixF(FDivLL(a, b)); }
        Group("float LL");
        foreach (float a in floats) { MixF(FAddLC(a)); MixF(FSubLC(a)); MixF(FMulLC(a)); MixF(FDivLC(a)); }
        Group("float LC");
        foreach (int a in ints) foreach (int b in ints) {
            Mix((uint)(BeqLL(a, b) + 2 * BneLL(a, b) + 4 * BltLL(a, b) + 8 * BleLL(a, b) + 16 * BgtLL(a, b) + 32 * BgeLL(a, b)));
        }
        Group("branch LL");
        foreach (int a in ints) {
            Mix((uint)(BeqLC(a) + 2 * BneLC(a) + 4 * BltLC(a) + 8 * BleLC(a) + 16 * BgtLC(a) + 32 * BgeLC(a)));
        }
        Group("branch LC");
        for (int n = 0; n < 40; n++) { Mix((uint)Counting(n)); Mix((uint)CountingBy(n)); Mix((uint)CountingDown(n)); }
        foreach (int a in ints) foreach (int b in ints) Mix((uint)Pick(a, b));
        Group("increments and calls");
        foreach (int a in ints) foreach (int b in ints) {
            Mix((uint)TernaryLeft(true, a, b, 1000)); Mix((uint)TernaryLeft(false, a, b, 1000));
            Mix((uint)TernaryRight(true, a, b, 1000)); Mix((uint)TernaryRight(false, a, b, 1000));
            Mix((uint)TernaryCompare(true, a, b, 5)); Mix((uint)TernaryCompare(false, a, b, 5));
            Mix((uint)NestedTernary(a, b, 3, 4)); Mix((uint)ShortCircuit(a, b, 9));
        }
        foreach (float a in floats) foreach (float b in floats) { MixF(TernaryFloat(true, a, b, 2f)); MixF(TernaryFloat(false, a, b, 2f)); }
        Group("targets inside a run");
        foreach (int a in ints) foreach (int b in ints) Mix((uint)InTry(a, b));
        Group("try blocks");
    }
}
