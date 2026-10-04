using System;

// Calls inlined into native blocks (see StencilBlocks.cs): a method whose whole body is one loop-free native block that calls and
// allocates nothing is made part of its caller's block, with its arguments, locals and result in an extension of the caller's frame.
// This checks the things that have to stay the same: argument sizes (4, 8, and a struct by value), instance methods on a class (callvirt
// still checks `this`) and on a struct, virtual calls (the target is the object's, so not inlined), callees that call callees,
// several inlines in one block sharing the frame, an exception thrown from inside an inlined callee, the order in which arguments are
// evaluated, callee locals, and generic methods. Compared with Mono line for line; the suite also runs it with DNA_NO_INLINE=1.
struct V3 {
    public float X, Y, Z;
    public V3(float x, float y, float z) { X = x; Y = y; Z = z; }
    public void Set(float x, float y, float z) { X = x; Y = y; Z = z; }
    public float LenSq() { return X * X + Y * Y + Z * Z; }
    public void AddScaled(float s) { X += s; Y += s * 2f; Z += s * 3f; }
}
class Counter {
    int n; long total; double mean;
    public int N { get { return n; } set { n = value; } }
    public long Total { get { return total; } }
    public void Add(int v) { n++; total += v; mean = (double)total / n; }
    public double Mean() { return mean; }
    public int Twice() { return Helper(n) + Helper(n + 1); }          // a callee that calls a callee
    int Helper(int x) { return x * 3 + 1; }
    public int Const() { return 42; }                                  // never touches `this`, but callvirt must still check it
    public virtual int Kind() { return 1; }
}
class Counter2 : Counter { public override int Kind() { return 2; } }
class Program {
    static uint h;
    static void Mix(uint v) { h = (h ^ v) * 16777619u; }
    static void MixL(long v) { Mix((uint)v); Mix((uint)(v >> 32)); }
    static void MixF(float f) { Mix((uint)(int)(f * 1000f)); }
    static void MixD(double d) { MixL((long)(d * 1000.0)); }
    static void Group(string name) { Console.WriteLine(name + " " + h); h = 2166136261u; }

    static int Sq(int a) { return a * a; }
    static int Mad(int a, int b, int c) { return a * b + c; }
    static float Lerp(float a, float b, float t) { return a + (b - a) * t; }
    static long Mix64(long a, long b) { return (a << 3) ^ (b + a); }
    static double Hyp(double a, double b) { return a * a + b * b; }
    static double Mixed(int i, double d, long l, float f) { return i + d * 2.0 + l + f; }
    static int Sign(int v) { int r; if (v < 0) r = -1; else if (v > 0) r = 1; else r = 0; return r; }
    static int Clamp(int v, int lo, int hi) { int r = v; if (r < lo) r = lo; if (r > hi) r = hi; return r; }
    static int Locals(int a) { int x = a + 1; int y = x * 2; int z = y - a; long w = (long)z * 7; double d = w; return x + y + z + (int)w + (int)d; }
    static int Locals2(int a) { int x = a + 1; int y = x * 2; int z = y - a; int w = z * 3; int v = w + x; return x + y + z + w + v; }     // 20 bytes of locals
    static float Local3(float s) { V3 v; v.X = s; v.Y = s * 2f; v.Z = s * 3f; return v.X + v.Y + v.Z + v.LenSq(); }     // the address of a local struct
    static int Nested(int a, int b) { return Sq(a) + Mad(a, b, Sq(b)); }              // inlines inlined callees
    static float DotV(V3 a, V3 b) { return a.X * b.X + a.Y * b.Y + a.Z * b.Z; }
    static float Pick(int[] a, int i) { return a[i] * 2; }                           // can throw inside the inlined callee
    static int Len(int[] a) { return a.Length; }
    static T Id<T>(T x) { return x; }
    static int Rec(int n) { return n <= 0 ? 0 : n + Rec(n - 1); }                     // recursion: never inlined
    static int NoLeaf(int a) { string s = "x" + a; return s.Length; }                // calls: not a leaf

    static int order;
    static int Next() { order = order * 10 + 1; return order; }
    static int Combine(int a, int b) { return a * 100000 + b; }

    public static void Main() {
        h = 2166136261u;
        // 4-byte, 8-byte and mixed arguments, results used in expressions with things already on the evaluation stack
        for (int i = -3; i < 40; i++) {
            Mix((uint)(Sq(i) + 1 + Mad(i, 3, i + 2) * 2));
            MixF(Lerp(i, 10f, 0.25f) + Lerp(2f, i, 0.5f));
            MixL(Mix64(i * 1000003L, (long)i << 20) + Mix64(5L, i));
            MixD(Hyp(i, 1.5) + Hyp(0.5, i * 2));
            MixD(Mixed(i, i * 0.5, (long)i * 100000, i / 3f));
        }
        Group("arguments");
        // callee locals, branches inside the callee, a callee that calls inlinable callees
        for (int i = -5; i < 30; i++) {
            Mix((uint)(Sign(i) + Sign(i - 10) * 3 + Clamp(i, -2, 12) * 5 + Clamp(i * 3, 0, 20)));
            Mix((uint)Locals(i)); Mix((uint)Locals2(i)); MixF(Local3(i * 0.25f)); Mix((uint)Nested(i, 7 - i));
        }
        Group("locals, branches, nesting");
        // several inlines in one block share one frame; arguments evaluated in order; one call as another's argument
        for (int i = 0; i < 20; i++) {
            int a = Sq(i) + Sq(i + 1) * Sq(i + 2) + Mad(Sq(i), Sq(2), Mad(1, 2, 3));
            Mix((uint)a);
            order = 0; int c = Combine(Next(), Next()); Mix((uint)c); Mix((uint)order);
            Mix((uint)Combine(Combine(i, i + 1), Combine(i + 2, Sq(i))));
        }
        Group("sharing the frame, evaluation order");
        // struct by value, and instance methods on a struct and on a class
        V3 p = new V3(1f, 2f, 3f), q = new V3(0.5f, -1f, 4f);
        Counter ctr = new Counter();
        for (int i = 0; i < 25; i++) {
            MixF(DotV(p, q) + DotV(q, new V3(i, 1f, 2f)));
            p.AddScaled(0.1f); q.Set(i, p.X, q.Z + 1f); MixF(p.LenSq() + q.LenSq());
            ctr.Add(i * 3); ctr.N = ctr.N + 1; MixL(ctr.Total + ctr.N); MixD(ctr.Mean()); Mix((uint)ctr.Twice());
        }
        Group("structs and classes");
        // callvirt of a non-virtual method checks `this`; virtual methods go to the object's own
        Counter nul = null;
        int caught = 0;
        for (int i = 0; i < 6; i++) {
            order = 5;
            try { ctr.Add(Sq(i)); order = order + nul.N + Sq(3); Mix(999u); } catch (NullReferenceException) { caught += 1 + order; }
            try { order = order * 2 + nul.Const(); Mix(998u); } catch (NullReferenceException) { caught += 100 + order; }
            Mix((uint)ctr.Const());
        }
        Mix((uint)caught);
        Counter[] objs = { new Counter(), new Counter2(), new Counter(), new Counter2() };
        for (int i = 0; i < 40; i++) { Mix((uint)objs[i & 3].Kind()); Mix((uint)objs[(i + 1) & 3].Twice()); }
        Group("null this and virtual calls");
        // an exception thrown from inside an inlined callee, and the state after catching it
        int[] arr = { 5, 6, 7 };
        int thrown = 0; float sum = 0;
        for (int i = -2; i < 6; i++) {
            try { sum += Pick(arr, i) + Len(arr); } catch (IndexOutOfRangeException) { thrown += 1 + i; }
        }
        Mix((uint)thrown); MixF(sum);
        try { Pick(null, 0); } catch (NullReferenceException) { Mix(77u); }
        Group("exceptions");
        // generic methods, recursion and methods with calls (not inlined) alongside inlined ones
        for (int i = 0; i < 10; i++) {
            Mix((uint)(Id<int>(i) + Id<int>(i * 2))); MixL(Id<long>((long)i << 33)); MixF(Id<float>(i * 0.5f)); MixD(Id<double>(i / 7.0));
            Mix((uint)(Rec(i) + NoLeaf(i * 111) + Sq(i)));
        }
        Group("generics, recursion, non-leaf");
    }
}
