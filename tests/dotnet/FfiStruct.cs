using System;
using System.Runtime.InteropServices;

// [DllImport] with structs (tests/ffi/mylib.json declares their C layout): by value as an argument and as a result, an array of them, a ref to one,
// and structs among more than six arguments. Small ones that a C call keeps in a register, 24-byte ones it keeps in memory, one with padding inside.
// Compared with Mono (same C as a shared library).
class Program {
    [StructLayout(LayoutKind.Sequential)] struct Pair { public int a; public float b; }
    [StructLayout(LayoutKind.Sequential)] struct Rgba { public int r, g, b, a; }
    [StructLayout(LayoutKind.Sequential)] struct V3 { public double x, y, z; }
    [StructLayout(LayoutKind.Sequential)] struct Xf { public uint id; public float x, y, angle; public int mode; }
    [StructLayout(LayoutKind.Sequential)] struct Padded { public int s; public long l; public float f; }

    [DllImport("mylib")] static extern Pair pair_make(int a, float b);
    [DllImport("mylib")] static extern int pair_sum(Pair p);
    [DllImport("mylib")] static extern Pair pair_swap(Pair p, int k);
    [DllImport("mylib")] static extern Rgba rgba_blend(Rgba x, Rgba y);
    [DllImport("mylib")] static extern V3 v3_add(V3 a, V3 b);
    [DllImport("mylib")] static extern double v3_dot(V3 a, V3 b);
    [DllImport("mylib")] static extern V3 v3_scale(V3 a, double k);
    [DllImport("mylib")] static extern V3 v3_make(double x, double y, double z);
    [DllImport("mylib")] static extern double v3_mix(V3 a, int i, V3 b, double d, int j, int k, int l, float m);
    [DllImport("mylib")] static extern float xf_sum(Xf x);
    [DllImport("mylib")] static extern Xf xf_make(uint id, float x, float y, float angle, int mode);
    [DllImport("mylib")] static extern int xf_apply(Xf[] sets, int n, float dt);
    [DllImport("mylib")] static extern void xf_fill(Xf[] sets, int n);
    [DllImport("mylib")] static extern void xf_init(out Xf x, int id);
    [DllImport("mylib")] static extern double padded_sum(Padded p);
    [DllImport("mylib")] static extern Padded padded_make(int s, long l, float f);

    static uint h;
    static void Mix(uint v) { h = (h ^ v) * 16777619u; }
    static void MixL(long v) { Mix((uint)v); Mix((uint)(v >> 32)); }
    static void MixD(double d) { MixL((long)(d * 1000)); }
    static void Group(string name) { Console.WriteLine(name + " " + h); h = 2166136261u; }

    public static void Main() {
        h = 2166136261u;
        for (int k = -4; k < 20; k++) {
            Pair p = pair_make(k, k * 0.5f);
            Mix((uint)p.a); MixD(p.b);
            Mix((uint)pair_sum(p));
            Pair q = pair_swap(p, k * 3);
            Mix((uint)q.a); MixD(q.b);
        }
        Group("Pair (8 bytes)");
        for (int k = 0; k < 40; k++) {
            Rgba x = new Rgba(); x.r = k * 7; x.g = 255 - k; x.b = k * k; x.a = k * 3;
            Rgba y = new Rgba(); y.r = k + 100; y.g = k; y.b = 200 - k * 4; y.a = 250 - k;
            Rgba r = rgba_blend(x, y);
            Mix((uint)r.r); Mix((uint)r.g); Mix((uint)r.b); Mix((uint)r.a);
        }
        Group("Rgba (16 bytes, two registers)");
        for (int k = 0; k < 30; k++) {
            V3 a = v3_make(k, k * 0.5, -k * 2.25);
            V3 b = new V3(); b.x = 1.5; b.y = k - 7; b.z = 0.125 * k;
            V3 s = v3_add(a, b);
            MixD(s.x); MixD(s.y); MixD(s.z);
            MixD(v3_dot(a, b));
            V3 c = v3_scale(s, 0.5 + k);
            MixD(c.x); MixD(c.y); MixD(c.z);
            MixD(v3_mix(a, k, b, k * 0.5, k + 1, 2 - k, k * 3, k / 4f));
        }
        Group("V3 (24 bytes, in memory)");
        for (int k = 0; k < 30; k++) {
            Xf x = xf_make((uint)k, k * 0.5f, -k, k * 0.25f, k % 4);
            MixD(xf_sum(x)); Mix(x.id); MixD(x.x); MixD(x.y); MixD(x.angle); Mix((uint)x.mode);
        }
        Group("Xf by value and as a result");
        Xf[] sets = new Xf[12];
        xf_fill(sets, sets.Length);
        for (int i = 0; i < sets.Length; i++) { Mix(sets[i].id); MixD(sets[i].x); MixD(sets[i].y); MixD(sets[i].angle); Mix((uint)sets[i].mode); }
        for (int n = 0; n <= 12; n += 3) Mix((uint)xf_apply(sets, n, 2.0f + n * 0.25f));
        Group("Xf[] (an array of structs)");
        for (int id = 0; id < 10; id++) {
            Xf x;
            xf_init(out x, id);
            Mix(x.id); MixD(x.x); MixD(x.y); MixD(x.angle); Mix((uint)x.mode);
        }
        Group("Xf by out");
        for (int k = -3; k < 15; k++) {
            Padded p = padded_make(k * 100, ((long)k << 36) + k, k * 0.75f);
            Mix((uint)p.s); MixL(p.l); MixD(p.f);
            MixD(padded_sum(p));
        }
        Group("Padded (padding inside)");
    }
}
