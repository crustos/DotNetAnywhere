// Scalar replacement of small structs and inlining of short methods, in the wasm JIT (native/src/WasmJIT.c): a struct is held in wasm locals, a
// pointer to one is symbolic, and methods without branches are inlined. Each case below is one way that could go wrong. The output must equal
// Mono's. Floats are printed through (double) (Single.ToString is wrong on wasm32 for some values, which has nothing to do with this).
using System;

struct V { public float X, Y, Z; public V(float x, float y, float z) { X = x; Y = y; Z = z; } public V(V o) { X = o.X + 1f; Y = o.Y + 1f; Z = o.Z + 1f; } }
struct Mixed { public int I; public long L; public double D; public float F; public int[] R; public Mixed(int i, long l, double d, float f, int[] r) { I = i; L = l; D = d; F = f; R = r; } }
struct Pair { public V A, B; public int Tag; }
struct Big { public float a, b, c, d, e, f, g, h, i, j, k, l, m, n; }        // more leaves than are held in locals: stays in memory

class WasmJitSroa {
    static double D(float f) { return f; }
    static string S(V v) { return D(v.X) + "," + D(v.Y) + "," + D(v.Z); }

    static V Add(V a, V b) { return new V(a.X + b.X, a.Y + b.Y, a.Z + b.Z); }
    static V Scale(V a, float s) { return new V(a.X * s, a.Y * s, a.Z * s); }
    static V Twice(V a) { return Add(a, a); }                                // inlined into an inlined chain
    static float Dot(V a, V b) { return a.X * b.X + a.Y * b.Y + a.Z * b.Z; }
    static V Swap(V a) { return new V(a.Z, a.X, a.Y); }
    static V FromLocals(float x) { V t = default(V); t.X = x; t.Y = t.X * 2f; return t; }       // a struct local of the inlined callee (zeroed each time)
    static float Sum(V v) { return v.X + v.Y + v.Z; }

    // the constructor taking a V: its argument and its result are the same type at the same stack depth
    static float CopyCtor(float x) { V a = new V(x, 2f, 3f); V b = new V(a); V c = new V(new V(b)); return a.X + b.X * 10f + c.X * 100f + c.Z; }
    static float Chain(float x) { V a = new V(x, 1f, -1f); return Sum(Twice(Add(Scale(a, 3f), Swap(a)))) + Dot(Twice(a), Twice(Swap(a))); }
    static float InLoop(int n) { float t = 0f; for (int i = 0; i < n; i++) { V v = FromLocals(i); t += v.X + v.Y + v.Z; V w = FromLocals(-i); t += w.Y - w.X; } return t; }

    // a struct value that flows across a branch (it has to be in memory there), and one that is built in each arm
    static V Pick(bool c, V a, V b) { return c ? a : b; }
    static V Choose(int n) { V r = new V(0f, 0f, 0f); for (int i = 0; i < n; i++) r = (i & 1) == 0 ? Add(r, new V(i, 1f, 0f)) : Scale(r, 0.5f); return r; }
    static float TernaryField(int n) { V a = new V(1f, 2f, 3f), b = new V(4f, 5f, 6f); float t = 0f; for (int i = 0; i < n; i++) t += (i % 3 == 0 ? a : b).Y + Pick(i % 2 == 0, a, b).Z; return t; }

    // methods with branches that change `this`, called on a struct held in locals, on an array element and on a field of a struct
    struct Clampable {
        public float X, Y;
        public void Clamp(float lo, float hi) { if (X < lo) X = lo; if (X > hi) X = hi; if (Y < lo) Y = lo; if (Y > hi) Y = hi; }
        public float Len2() { return X * X + Y * Y; }
        public void Add(float dx) { X += dx; Y -= dx; }
    }
    struct Holder { public Clampable C; public int N; }
    static float Mutate(float x) {
        Clampable c = default(Clampable); c.X = x; c.Y = -x;
        c.Clamp(-1f, 1f); c.Add(0.25f);
        Holder h = default(Holder); h.C.X = x * 3f; h.C.Y = 9f; h.C.Clamp(0f, 2f); h.C.Add(1f); h.N = 7;
        Clampable[] arr = new Clampable[3]; arr[1].X = x; arr[1].Y = x * 2f; arr[1].Clamp(-0.5f, 0.5f); arr[1].Add(0.125f);
        return c.X * 1000f + c.Y * 100f + h.C.X * 10f + h.C.Y + h.N + arr[1].X * 0.1f + arr[1].Y * 0.01f + c.Len2() + arr[1].Len2();
    }

    // every kind of leaf, a reference among them, nested structs, and one too big to be held in locals
    static double Leaves(int i) {
        Mixed m = new Mixed(i, (long)i * 1000000007L, i * 0.5, i * 0.25f, new int[(i & 7) + 1]), n = m;
        n.I += 2; n.L -= 3; n.D *= 2.0; n.F += 1f;
        Pair p = default(Pair); p.A = new V(i, 1f, 2f); p.B = Add(p.A, new V(1f, 1f, 1f)); p.Tag = i; Pair q = p; q.A.X += 100f; q.Tag++;
        Big big = default(Big); big.a = i; big.n = i * 2; big.g = big.a + big.n; Big big2 = big; big2.h = 5f;
        return m.I + m.L + m.D + m.F + n.I + n.L + n.D + n.F + (m.R == null ? 0 : m.R.Length + n.R.Length) + p.A.X + p.B.X + p.Tag + q.A.X + q.B.Y + q.Tag + big.g + big2.h + big2.n;
    }

    // arrays of structs in and out of the scalar form
    static float Arrays(int n) {
        V[] vs = new V[n]; Pair[] ps = new Pair[n];
        for (int i = 0; i < n; i++) { vs[i] = new V(i, i * 2f, i * 3f); ps[i].A = vs[i]; ps[i].B = Scale(vs[i], 2f); ps[i].Tag = i; }
        float t = 0f; for (int i = 0; i < n; i++) { V v = vs[i]; v.X += 1f; vs[i] = Add(v, ps[i].B); t += ps[i].A.Y + ps[i].B.Z + ps[i].Tag + vs[i].X; }
        return t;
    }

    // exceptions out of inlined code, with a struct on the stack
    static float Throws(V[] arr, int i) { V v = arr[i]; return Dot(v, Add(arr[0], v)); }
    static float NullArg(Mixed[] arr) { Mixed m = arr[0]; return m.I; }
    static string Try(Func<float> f) { try { return D(f()).ToString(); } catch (IndexOutOfRangeException) { return "IOOR"; } catch (NullReferenceException) { return "NRE"; } }

    static void Main() {
        for (float x = -2f; x <= 2f; x += 1f) Console.WriteLine("copyctor " + D(CopyCtor(x)) + " chain " + D(Chain(x)) + " loop " + D(InLoop((int)(x + 3f))));
        Console.WriteLine("choose " + S(Choose(0)) + " " + S(Choose(7)) + " " + S(Choose(20)) + " ternary " + D(TernaryField(10)));
        for (float x = -3f; x < 4f; x += 1.5f) Console.WriteLine("mutate " + D(Mutate(x)));
        for (int i = 0; i < 5; i++) Console.WriteLine("leaves " + Leaves(i) + " " + Leaves(-i));
        Console.WriteLine("arrays " + D(Arrays(10)) + " " + D(Arrays(1)) + " " + D(Arrays(0)));
        V[] a = new V[3]; for (int i = 0; i < 3; i++) a[i] = new V(i, 1f, 2f);
        Console.WriteLine("throws " + Try(() => Throws(a, 1)) + " " + Try(() => Throws(a, 3)) + " " + Try(() => Throws(null, 0)) + " " + Try(() => NullArg(null)) + " " + Try(() => NullArg(new Mixed[0])));
    }
}
