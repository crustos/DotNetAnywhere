using System;

// Straight-line float32 code, the kind that vector math is made of, and the one shape that must not become a native
// block (a ternary: a jump target in the middle of the expression). The interpreter compiles a run of loads, stores,
// constants and float32 arithmetic to native code by copying stencils (tools/gen_stencils.py); the results must be bit
// for bit what Mono gives, including NaNs, infinities, negative zero and denormals. Compared with Mono line for line;
// the test suite also runs it with DNA_NO_STENCILS=1.
class Program {
    static uint h;
    static void Mix(uint v) { h = (h ^ v) * 16777619u; }
    static unsafe void MixF(float f) { if (f != f) { Mix(0x7fc00000u); } else { Mix(*(uint*)&f); } }

    static float Dot3(float ax, float ay, float az, float bx, float by, float bz) { return ax * bx + ay * by + az * bz; }
    static float CrossX(float ay, float az, float by, float bz) { return ay * bz - az * by; }
    static float Lerp(float a, float b, float t) { return a + (b - a) * t; }
    static float Lerp2(float a, float b, float t) { return a * (1f - t) + b * t; }
    static float Poly(float x) { return ((0.5f * x + 1.25f) * x - 3.0f) * x + 7.0f; }
    static float Smooth(float t) { return t * t * (3f - 2f * t); }
    static float Neg(float a, float b) { return -(a * b) + -a - -b; }
    static float Mat(float m0, float m1, float m2, float m3, float v0, float v1, float v2, float v3) { return m0 * v0 + m1 * v1 + m2 * v2 + m3 * v3; }
    static float Accum(float a, float b, float c) {
        float s = a; s = s + b * c; s = s - c * a; s = s * 0.5f;
        float t = s + 1f; t = t / (b + 2f);
        return s + t;
    }
    // 7 arguments and 3 locals: slots past the first 8 use the generic offset form
    static float Quat(float qx, float qy, float qz, float qw, float vx, float vy, float vz) {
        float tx = 2f * (qy * vz - qz * vy);
        float ty = 2f * (qz * vx - qx * vz);
        float tz = 2f * (qx * vy - qy * vx);
        return vx + qw * tx + (qy * tz - qz * ty);
    }
    // a long run: several statements, nothing that branches
    static float Long(float a, float b, float c, float d) {
        float e = a * b; float f = c * d; float g = e + f; float i = e - f;
        float j = g * i; float k = j / (a + 3f); float l = -k + b;
        float m = l * l + k * k; float n = m - 0.125f * j;
        return n * 2f + g - i;
    }
    // a jump target inside the expression: the first operand arrives by two paths
    static float Ternary(bool k, float a, float b, float c) { return (k ? a : b) * c + a * b * c * 2f; }
    static float TernaryAfter(bool k, float a, float b, float c) { float x = a * b + c; x = x * 2f + a; float y = k ? x : b; return y * c + a * b - c; }
    // ints go through the same loads, stores and constants
    static int IntMix(int a, int b) { int c = a; int d = b; int e = c; return e + d; }

    static readonly float[] vals = { 0f, -0f, 1f, -1f, 0.5f, 3.14159f, 1e20f, -1e20f, 1e-20f, float.NaN,
        float.PositiveInfinity, float.NegativeInfinity, 16777216f, 7f, -2.5f, 1e-40f, 3e38f };

    static void Group(string name) { Console.WriteLine(name + " " + h); h = 2166136261u; }

    public static void Main() {
        h = 2166136261u;
        for (int i = 0; i < vals.Length; i++) for (int j = 0; j < vals.Length; j++) {
            float a = vals[i], b = vals[j], c = vals[(i + j) % vals.Length];
            MixF(Dot3(a, b, c, b, c, a)); MixF(CrossX(a, b, c, a)); MixF(Lerp(a, b, c)); MixF(Lerp2(a, b, c));
            MixF(Smooth(a)); MixF(Neg(a, b));
        }
        Group("dot cross lerp smooth neg");
        foreach (float x in vals) { MixF(Poly(x)); }
        Group("polynomial");
        for (int i = 0; i < vals.Length; i++) for (int j = 0; j < vals.Length; j++) {
            float a = vals[i], b = vals[j], c = vals[(i * 3 + j) % vals.Length], d = vals[(i + 2 * j) % vals.Length];
            MixF(Mat(a, b, c, d, d, c, b, a)); MixF(Accum(a, b, c)); MixF(Long(a, b, c, d));
            MixF(Quat(a, b, c, d, b, c, a));
        }
        Group("matrix accum long quat");
        for (int i = 0; i < vals.Length; i++) for (int j = 0; j < vals.Length; j++) {
            float a = vals[i], b = vals[j], c = vals[(i + j) % vals.Length];
            MixF(Ternary(true, a, b, c)); MixF(Ternary(false, a, b, c)); MixF(TernaryAfter(true, a, b, c)); MixF(TernaryAfter(false, a, b, c));
        }
        Group("jump target inside");
        for (int i = -5; i < 40; i++) Mix((uint)IntMix(i * 1000003, i - 7));
        Group("ints");
    }
}
