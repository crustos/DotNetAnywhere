using System;

// Field access in native blocks (see StencilBlocks.cs): Vector3-style structs held in locals and arguments, ref
// parameters, classes with float fields, `+=` on fields, and null references. A native block cannot throw, so a null
// object makes it exit with a status and the interpreter throws NullReferenceException; the exception must come from the
// right instruction, with everything before it done and nothing after it. Compared with Mono line for line; the suite
// also runs it with DNA_NO_STENCILS=1.
struct V3 {
    public float X, Y, Z;
    public V3(float x, float y, float z) { X = x; Y = y; Z = z; }
}

class Particle {
    public float X, Y, Z, VX, VY, VZ, Mass;
    public void Step(float dt) {
        X += VX * dt; Y += VY * dt; Z += VZ * dt;
        VX *= 0.99f; VY *= 0.99f; VZ -= 9.8f * dt;
    }
    public float Energy() { return 0.5f * Mass * (VX * VX + VY * VY + VZ * VZ); }
}

class Node { public float A, B; public int Count; public Node Next; }

class Program {
    static uint h;
    static void Mix(uint v) { h = (h ^ v) * 16777619u; }
    static unsafe void MixF(float f) { if (f != f) { Mix(0x7fc00000u); } else { Mix(*(uint*)&f); } }
    static void MixV(V3 v) { MixF(v.X); MixF(v.Y); MixF(v.Z); }
    static void Group(string name) { Console.WriteLine(name + " " + h); h = 2166136261u; }

    static V3 AddV(V3 a, V3 b) { V3 r; r.X = a.X + b.X; r.Y = a.Y + b.Y; r.Z = a.Z + b.Z; return r; }
    static float DotV(V3 a, V3 b) { return a.X * b.X + a.Y * b.Y + a.Z * b.Z; }
    static V3 CrossV(V3 a, V3 b) {
        V3 r;
        r.X = a.Y * b.Z - a.Z * b.Y; r.Y = a.Z * b.X - a.X * b.Z; r.Z = a.X * b.Y - a.Y * b.X;
        return r;
    }
    static V3 LerpV(V3 a, V3 b, float t) {
        V3 r;
        r.X = a.X + (b.X - a.X) * t; r.Y = a.Y + (b.Y - a.Y) * t; r.Z = a.Z + (b.Z - a.Z) * t;
        return r;
    }
    static float LenSq(ref V3 v) { return v.X * v.X + v.Y * v.Y + v.Z * v.Z; }
    static void Scale(ref V3 v, float k) { v.X *= k; v.Y *= k; v.Z *= k; }
    static void Integrate(ref V3 p, ref V3 v, V3 a, float dt) {
        v.X = (v.X + a.X * dt) * 0.999f; v.Y = (v.Y + a.Y * dt) * 0.999f; v.Z = (v.Z + a.Z * dt) * 0.999f;
        p.X = p.X + v.X * dt; p.Y = p.Y + v.Y * dt; p.Z = p.Z + v.Z * dt;
    }
    // a struct local assembled and used in one method
    static float LocalStruct(float x, float y, float z) {
        V3 a = new V3(x, y, z);
        V3 b; b.X = y; b.Y = z; b.Z = x;
        a.X = a.X * 2f + b.X; a.Y = a.Y * 2f + b.Y; a.Z = a.Z * 2f + b.Z;
        return a.X * b.X + a.Y * b.Y + a.Z * b.Z + a.X;
    }

    // null references: the read, the write, and what must and must not have happened before the fault
    static float NullRead(Node n, float x) { float t = x * 2f; t = t + 1f; float r = n.A * t + n.B; return r + 7f; }
    static float NullWrite(Node n, Node good, float x) {
        good.A = x; good.B = x * 2f;          // done before the fault
        n.A = x + 1f;                         // faults when n is null
        good.A = 99f;                         // must not happen then
        return x;
    }
    static float Chain(Node n) { float a = n.A + 1f; a = a * 2f; a = a + n.B; return a; }

    public static void Main() {
        h = 2166136261u;
        float[] vals = { 0f, -0f, 1f, -1f, 0.5f, 3.14159f, 1e20f, -1e20f, 1e-20f, float.NaN, float.PositiveInfinity, -2.5f, 7f };
        for (int i = 0; i < vals.Length; i++) for (int j = 0; j < vals.Length; j++) {
            V3 a = new V3(vals[i], vals[j], vals[(i + j) % vals.Length]);
            V3 b = new V3(vals[j], vals[(i * 2 + j) % vals.Length], vals[i]);
            MixV(AddV(a, b)); MixF(DotV(a, b)); MixV(CrossV(a, b)); MixV(LerpV(a, b, vals[(i + 2 * j) % vals.Length]));
            MixF(LenSq(ref a)); MixF(LocalStruct(vals[i], vals[j], 2f));
        }
        Group("struct locals and arguments");
        for (int i = 0; i < vals.Length; i++) for (int j = 0; j < vals.Length; j++) {
            V3 p = new V3(vals[i], 1f, 2f), v = new V3(vals[j], -1f, 0.5f), a = new V3(0.5f, vals[(i + j) % vals.Length], -9.8f);
            Integrate(ref p, ref v, a, 0.01f); Scale(ref p, vals[j]); Scale(ref v, 0.5f);
            MixV(p); MixV(v);
        }
        Group("ref parameters and +=");
        Particle[] ps = new Particle[8];
        for (int i = 0; i < ps.Length; i++) { ps[i] = new Particle(); ps[i].X = i; ps[i].VX = vals[i]; ps[i].VY = vals[(i + 3) % vals.Length]; ps[i].VZ = 2f; ps[i].Mass = 1.5f; }
        for (int step = 0; step < 30; step++) for (int i = 0; i < ps.Length; i++) ps[i].Step(0.016f);
        for (int i = 0; i < ps.Length; i++) { MixF(ps[i].X); MixF(ps[i].Y); MixF(ps[i].Z); MixF(ps[i].VX); MixF(ps[i].Energy()); }
        Group("class fields");

        // null references
        Node good = new Node(); good.A = 1.5f; good.B = 0.25f;
        float r1 = NullRead(good, 3f);
        Console.WriteLine("read through a real object: " + r1);
        try { NullRead(null, 3f); Console.WriteLine("no exception"); }
        catch (NullReferenceException) { Console.WriteLine("read through null: NullReferenceException"); }
        good.A = 0f; good.B = 0f;
        try { NullWrite(null, good, 5f); Console.WriteLine("no exception"); }
        catch (NullReferenceException) { Console.WriteLine("write through null: NullReferenceException, good.A=" + good.A + " good.B=" + good.B); }
        good.A = 0f; good.B = 0f;
        float r2 = NullWrite(new Node(), good, 5f);
        Console.WriteLine("write through a real object: " + r2 + " good.A=" + good.A + " good.B=" + good.B);
        int caught = 0; float total = 0f;
        Node mid = new Node(); mid.A = 2f; mid.B = 3f;
        for (int i = 0; i < 20; i++) {
            try { total += Chain(i % 3 == 0 ? null : mid); } catch (NullReferenceException) { caught++; }
        }
        Console.WriteLine("repeated: caught " + caught + ", total " + total);
        try { try { Chain(null); } finally { Console.WriteLine("finally ran"); } } catch (Exception e) { Console.WriteLine("outer caught " + e.GetType().Name); }
    }
}
