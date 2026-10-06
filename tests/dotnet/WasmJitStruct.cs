// Value types, field access and calls between methods, for the wasm JIT (native/src/WasmJIT.c). The output must equal Mono's. Floats are printed
// through (double): Single.ToString is wrong on wasm32 for large and small values, which has nothing to do with what is tested here.
using System;

struct Vec3 {
    public float X, Y, Z;
    public Vec3(float x, float y, float z) { X = x; Y = y; Z = z; }
    public static Vec3 operator +(Vec3 a, Vec3 b) { return new Vec3(a.X + b.X, a.Y + b.Y, a.Z + b.Z); }
    public static Vec3 operator -(Vec3 a, Vec3 b) { return new Vec3(a.X - b.X, a.Y - b.Y, a.Z - b.Z); }
    public static Vec3 operator *(Vec3 a, float s) { return new Vec3(a.X * s, a.Y * s, a.Z * s); }
    public static float Dot(Vec3 a, Vec3 b) { return a.X * b.X + a.Y * b.Y + a.Z * b.Z; }
    public static Vec3 Cross(Vec3 a, Vec3 b) { return new Vec3(a.Y * b.Z - a.Z * b.Y, a.Z * b.X - a.X * b.Z, a.X * b.Y - a.Y * b.X); }
    public float Length() { return (float)Math.Sqrt(Dot(this, this)); }
    public Vec3 Normalized() { float l = Length(); return l > 0 ? this * (1f / l) : this; }
    public void Scale(float s) { X *= s; Y *= s; Z *= s; }          // changes `this`
    public float Sum { get { return X + Y + Z; } }
}

struct Mixed { public int A; public long B; public double C; public byte D; public short E; public bool F; public char G; public float H; }
struct Particle { public Vec3 Pos; public Vec3 Vel; public float Mass; public int Id; }

class Body {
    public float x, y, vx, vy; public int hits; public Body next;
    public void Step(float dt) { x += vx * dt; y += vy * dt; if (y < 0f) { y = -y; vy = -vy; hits++; } }
    public float Energy() { return 0.5f * (vx * vx + vy * vy) + y * Gravity; }
    public static float Gravity = 9.8f;
    public static int Counter;
}

class WasmJitStruct {
    static double D(float f) { return f; }
    static string S(Vec3 v) { return D(v.X) + "," + D(v.Y) + "," + D(v.Z); }

    // ---- value semantics
    static float Copies(float x) {
        Vec3 a = new Vec3(x, 2f, 3f);
        Vec3 b = a;                 // a copy
        b.X = 100f; b.Y += 1f;
        Vec3 c = default(Vec3);
        c.Z = a.X + b.X;
        return a.X * 1000f + a.Y * 100f + b.X + b.Y * 10f + c.Z + c.X + c.Y;
    }
    static Vec3 Mutate(Vec3 v) { v.X = 77f; v.Scale(2f); return v; }       // the caller's value does not change
    static float Callee(float x) { Vec3 a = new Vec3(x, x + 1f, x + 2f); Vec3 b = Mutate(a); return a.X + a.Y * 10f + a.Z * 100f + b.X * 1000f + b.Y; }

    // ---- struct arithmetic through operators and calls
    static Vec3 Combine(Vec3 a, Vec3 b, float s) { return (a + b) * s - Vec3.Cross(a, b); }
    static float Lengths(float x, float y, float z) { Vec3 v = new Vec3(x, y, z); return v.Length() + v.Normalized().Length() + Vec3.Dot(v, v.Normalized()) + v.Sum; }
    static Vec3 Chain(int n) {
        Vec3 acc = new Vec3(1f, 0f, -1f), step = new Vec3(0.5f, 0.25f, 0.125f);
        for (int i = 0; i < n; i++) { acc = acc + step * (float)i; acc = Combine(acc, step, 0.5f) * 0.5f; }   // struct locals live across iterations (and yields)
        return acc;
    }
    static float LongLoop(int n) {
        Vec3 v = new Vec3(1f, 2f, 3f); float t = 0f;
        for (int i = 0; i < n; i++) { v = v + new Vec3(0.001f, 0.002f, 0.003f); if ((i & 1023) == 0) { t += v.Length(); v = v.Normalized() * 3f; } }
        return t + v.X + v.Y + v.Z;
    }

    // ---- structs with fields of every size, and nested structs
    static double MixedOps(int i) {
        Mixed m = default(Mixed);
        m.A = i; m.B = (long)i * 1000000007L; m.C = i * 0.5; m.D = (byte)(i * 3); m.E = (short)(i * 1000); m.F = (i & 1) == 0; m.G = (char)(65 + i % 26); m.H = i * 0.25f;
        Mixed n = m; n.A += 1; n.B -= 5;
        return m.A + m.B + m.C + m.D + m.E + (m.F ? 1 : 0) + m.G + m.H + n.A * 3 + n.B + (n.F ? 7 : 8);
    }
    static float Nested(float s) {
        Particle p = default(Particle);
        p.Pos = new Vec3(s, s * 2f, s * 3f); p.Vel.X = 1f; p.Vel.Y = -1f; p.Mass = s + 1f; p.Id = (int)s;
        Particle q = p;                       // copies the nested structs too
        q.Pos.X += 10f; q.Vel = q.Vel * 2f;
        return p.Pos.X + p.Pos.Y * 10f + p.Vel.X * 100f + q.Pos.X * 1000f + q.Vel.X + q.Vel.Y + q.Mass + q.Id;
    }

    // ---- arrays of structs
    static float ParticlesStep(Particle[] ps, float dt, int steps) {
        for (int s = 0; s < steps; s++)
            for (int i = 0; i < ps.Length; i++) {
                ps[i].Vel.Y += -9.8f * dt;
                ps[i].Pos = ps[i].Pos + ps[i].Vel * dt;
                if (ps[i].Pos.Y < 0f) { ps[i].Pos.Y = -ps[i].Pos.Y; ps[i].Vel.Y = -ps[i].Vel.Y * 0.8f; }
            }
        float sum = 0f; for (int i = 0; i < ps.Length; i++) sum += ps[i].Pos.X + ps[i].Pos.Y + ps[i].Pos.Z + ps[i].Mass;
        return sum;
    }
    static Vec3 SumVecs(Vec3[] vs) { Vec3 t = default(Vec3); for (int i = 0; i < vs.Length; i++) t = t + vs[i]; return t; }
    static void SwapEnds(Vec3[] vs) { Vec3 tmp = vs[0]; vs[0] = vs[vs.Length - 1]; vs[vs.Length - 1] = tmp; }
    static void NormalizeAll(Vec3[] vs) { for (int i = 0; i < vs.Length; i++) vs[i] = vs[i].Normalized(); }

    // ---- fields of classes, statics
    static float Bodies(Body[] bs, int steps, float dt) {
        for (int s = 0; s < steps; s++) for (int i = 0; i < bs.Length; i++) bs[i].Step(dt);
        float e = 0f; int h = 0;
        for (int i = 0; i < bs.Length; i++) { e += bs[i].Energy(); h += bs[i].hits; }
        Body.Counter += h;
        return e;
    }
    static int Chase(Body b) { int n = 0; while (b != null) { n += b.hits; b = b.next; } return n; }
    static float UseStatic(float x) { Body.Gravity = x * 2f; Body.Counter += 5; return Body.Gravity + Body.Counter; }

    // ---- exceptions raised several calls down
    static float Deep3(Body b, int[] a, int i, int d) { return b.x + a[i] + 100 / d; }
    static float Deep2(Body b, int[] a, int i, int d) { return Deep3(b, a, i, d) * 2f; }
    static float Deep1(Body b, int[] a, int i, int d) { Vec3 v = new Vec3(1f, 2f, 3f); return Deep2(b, a, i, d) + v.X; }
    static string Try(Body b, int[] a, int i, int d) {
        try { return D(Deep1(b, a, i, d)).ToString(); }
        catch (NullReferenceException) { return "NRE"; } catch (IndexOutOfRangeException) { return "IOOR"; } catch (DivideByZeroException) { return "DIV0"; }
    }

    static void Main() {
        for (float x = -2f; x < 3f; x += 0.75f) Console.WriteLine("copies " + D(Copies(x)) + " " + D(Callee(x)));
        Vec3 va = new Vec3(1f, 2f, 3f), vb = new Vec3(-0.5f, 4f, 0.25f);
        Console.WriteLine("combine " + S(Combine(va, vb, 1.5f)) + " " + S(Combine(vb, va, -2f)));
        for (float x = -3f; x <= 3f; x += 1.5f) Console.WriteLine("lengths " + D(Lengths(x, 1f, -2f)) + " " + D(Lengths(0f, 0f, x)));
        Console.WriteLine("chain " + S(Chain(10)) + " " + S(Chain(0)) + " " + S(Chain(100)));
        Console.WriteLine("longloop " + D(LongLoop(100000)));
        for (int i = 0; i < 8; i++) Console.WriteLine("mixed " + MixedOps(i) + " " + MixedOps(-i));
        for (float s = 0f; s < 4f; s += 1f) Console.WriteLine("nested " + D(Nested(s)));

        Particle[] ps = new Particle[50];
        for (int i = 0; i < ps.Length; i++) { ps[i].Pos = new Vec3(i, 10f + i % 7, -i); ps[i].Vel = new Vec3(1f, 0f, 0.5f); ps[i].Mass = 1f + i; ps[i].Id = i; }
        Console.WriteLine("particles " + D(ParticlesStep(ps, 0.016f, 200)) + " " + S(ps[7].Pos) + " " + S(ps[7].Vel));
        Vec3[] vs = new Vec3[20];
        for (int i = 0; i < vs.Length; i++) vs[i] = new Vec3(i, i * 0.5f, 2f - i);
        Console.WriteLine("sumvecs " + S(SumVecs(vs)));
        SwapEnds(vs); Console.WriteLine("swap " + S(vs[0]) + " " + S(vs[19]));
        NormalizeAll(vs); Console.WriteLine("norm " + S(vs[3]) + " " + S(vs[19]));

        Body[] bs = new Body[10];
        for (int i = 0; i < bs.Length; i++) { bs[i] = new Body(); bs[i].x = i; bs[i].y = 1f + i; bs[i].vx = 1f; bs[i].vy = -2f - i * 0.5f; if (i > 0) bs[i - 1].next = bs[i]; }
        Console.WriteLine("bodies " + D(Bodies(bs, 500, 0.01f)) + " hits " + Chase(bs[0]) + " counter " + Body.Counter);
        Console.WriteLine("static " + D(UseStatic(1.5f)) + " " + D(UseStatic(2f)) + " " + D(Body.Gravity) + " " + Body.Counter);

        int[] arr = new int[5]; for (int i = 0; i < 5; i++) arr[i] = i * 10;
        Body ok = new Body(); ok.x = 1.5f;
        string line = "exceptions";
        foreach (int i in new int[] { 0, 4, 5, -1 }) foreach (int d in new int[] { 3, 0, -1 }) line += " " + Try(ok, arr, i, d);
        line += " " + Try(null, arr, 0, 1) + " " + Try(ok, null, 0, 1);
        Console.WriteLine(line);
    }
}
