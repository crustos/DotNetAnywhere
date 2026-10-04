using System;

// 8-byte fields (long, double, references) and the addresses of fields, in native blocks (see StencilFields.cs for the 4-byte ones):
// loading and storing them on classes and structs, following a chain of references (a null in it must throw at the same point),
// the address of a struct field inside an object, and the conversions between long and float / double. Compared with Mono line for
// line; the suite also runs it with DNA_NO_STENCILS=1 and DNA_NO_INLINE=1.
struct P2 { public float X; public long Z; public double W; public int K; }
class Node {
    public Node next; public Node other;
    public long id; public double w; public object tag; public int cnt; public float f;
    public P2 v;
}
struct S { public long a; public double b; public Node n; public int c; }
class Program {
    static uint h;
    static void Mix(uint v) { h = (h ^ v) * 16777619u; }
    static void MixL(long v) { Mix((uint)v); Mix((uint)(v >> 32)); }
    static void MixD(double d) { if (d != d) { Mix(0x7ff80000u); } else { MixL((long)(d * 1000.0)); } }
    static void Group(string name) { Console.WriteLine(name + " " + h); h = 2166136261u; }

    static long SumIds(Node n) { long s = 0; while (n != null) { s += n.id * 3 + n.cnt; n = n.next; } return s; }
    static double SumW(Node n, int limit) { double s = 0.0; int i = 0; while (n != null && i < limit) { s = s * 0.5 + n.w; n = n.next; i++; } return s; }
    static int Count(Node n) { int c = 0; while (n != null) { c++; n = n.other != null ? n.other : n.next; } return c; }
    static void Update(Node o, int k) {
        for (int i = 0; i < k; i++) { o.id += i * 7L; o.w = o.w * 0.5 + i; o.cnt += i; o.f = o.f + 0.25f; }
    }
    static void Link(Node a, Node b, Node c) { a.next = b; b.next = c; a.other = c; a.tag = b; if (c != null) c.tag = a; }
    static long Deep(Node n) { return n.next.next.id + n.next.id; }                   // a null at either step throws
    static float Inner(Node n, int k) { n.v.X += k; n.v.Z += (long)k * 1000003; n.v.W = n.v.W * 1.5 + k; n.v.K ^= k; return n.v.X + n.v.K; }
    static long InnerL(Node n) { return n.v.Z + n.v.K; }
    static double InnerD(Node n) { return n.v.W; }
    static long Structs(S s, int k) { s.a += k; s.b = s.b * 2.0 + k; s.c += k; S t = s; t.a ^= 0x5555555555L; s.n = t.n; return s.a + t.a + (long)s.b + s.c; }
    static long FromLongs(long v) { double d = (double)v; float f = (float)v; return (long)d + (long)f + (long)(d * 0.5); }
    static long ToLong(double d, float f) { long a = (long)d; long b = (long)f; return a * 3 + b; }

    public static void Main() {
        h = 2166136261u;
        // a chain of references: pointer-chasing loops, 8-byte loads and stores, a reference stored into fields
        Node[] ns = new Node[12];
        for (int i = 0; i < ns.Length; i++) { ns[i] = new Node(); ns[i].id = (long)i << 31 | i; ns[i].w = i * 0.75; ns[i].cnt = i * 5; }
        for (int i = 0; i + 1 < ns.Length; i++) { Link(ns[i], ns[i + 1], i + 2 < ns.Length ? ns[i + 2] : null); }
        for (int i = 0; i < ns.Length; i++) { MixL(SumIds(ns[i])); MixD(SumW(ns[i], 3)); MixD(SumW(ns[i], 100)); Mix((uint)Count(ns[i])); }
        Mix((uint)(ns[3].tag == ns[2] ? 1 : 0)); Mix((uint)(ns[3].next == ns[4] ? 1 : 0)); Mix((uint)(ns[11].next == null ? 1 : 0));
        Group("chains");
        // updating 8-byte fields in loops
        for (int i = 0; i < ns.Length; i++) { Update(ns[i], i + 3); MixL(ns[i].id); MixD(ns[i].w); Mix((uint)ns[i].cnt); Mix((uint)(int)(ns[i].f * 100f)); }
        Group("updates");
        // a null in a chain must throw at the same point, with the same state
        int caught = 0; long got = 0;
        for (int i = 0; i < ns.Length; i++) {
            try { got += Deep(ns[i]); } catch (NullReferenceException) { caught += 1 + i; }
            try { got += SumIds(null) + ns[i].next.next.next.id; } catch (NullReferenceException) { caught += 100; }
        }
        Mix((uint)caught); MixL(got);
        Node nul = null;
        try { nul.id = 5; } catch (NullReferenceException) { Mix(1u); }
        try { nul.w = 2.5; } catch (NullReferenceException) { Mix(2u); }
        try { nul.tag = ns; } catch (NullReferenceException) { Mix(3u); }
        try { MixL(nul.id); } catch (NullReferenceException) { Mix(4u); }
        try { Inner(nul, 1); } catch (NullReferenceException) { Mix(5u); }
        Group("nulls");
        // the address of a struct field inside an object
        for (int i = 0; i < ns.Length; i++) { Mix((uint)(int)(Inner(ns[i], i) * 10f)); MixL(InnerL(ns[i])); MixD(InnerD(ns[i])); }
        Group("addresses of fields");
        // structs on the frame
        for (int i = 0; i < 20; i++) {
            S s; s.a = (long)i << 40; s.b = i * 0.1; s.n = ns[i % ns.Length]; s.c = i;
            MixL(Structs(s, i)); MixL(s.a); Mix((uint)(s.n == ns[i % ns.Length] ? 1 : 0));
        }
        Group("structs");
        // long <-> float / double
        long[] ls = { 0, 1, -1, 123456789012345L, -987654321098765L, long.MaxValue, long.MinValue, 1L << 53, (1L << 53) + 1, 16777217, (1L << 60) + (1L << 36) + 1, (1L << 60) + (1L << 36), -((1L << 60) + (1L << 36) + 1) };
        double[] ds = { 0.0, -0.0, 1.5, -1.5, 1e10, -1e10, 4.5e15, 9.3e18, 1e30, double.NaN, double.PositiveInfinity, 0.999999, 1234567.891 };
        foreach (long l in ls) MixL(FromLongs(l));
        foreach (double d in ds) { MixL(ToLong(d, (float)d)); MixL(ToLong(d / 3.0, (float)(d * 0.7))); }
        Group("conversions");
    }
}
