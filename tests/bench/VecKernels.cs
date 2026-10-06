// Vector math the way game code writes it: a Vec3 struct with operators, used in loops over plain arrays and arrays of structs.
using System;
using System.Diagnostics;

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
}
struct Particle { public Vec3 Pos, Vel; public float Mass; }

class VecKernels {
    // cross / normalize / accumulate: the typical inner loop of vector code
    static float VecOps(int n) {
        Vec3 acc = new Vec3(0, 0, 0), a = new Vec3(1, 2, 3), b = new Vec3(0.5f, -1f, 2f);
        for (int i = 0; i < n; i++) {
            Vec3 c = Vec3.Cross(a, b);
            acc = acc + c.Normalized() * 0.001f;
            a = a + b * 0.0001f;
        }
        return acc.X + acc.Y + acc.Z + Vec3.Dot(a, b);
    }

    // an array of particle structs integrated under gravity, with a floor bounce
    static float Particles(Particle[] ps, int steps) {
        Vec3 g = new Vec3(0f, -9.8f, 0f); float dt = 0.016f;
        for (int s = 0; s < steps; s++)
            for (int i = 0; i < ps.Length; i++) {
                ps[i].Vel = ps[i].Vel + g * dt;
                ps[i].Pos = ps[i].Pos + ps[i].Vel * dt;
                if (ps[i].Pos.Y < 0f) { ps[i].Pos.Y = -ps[i].Pos.Y; ps[i].Vel.Y = -ps[i].Vel.Y * 0.8f; }
            }
        float sum = 0f; for (int i = 0; i < ps.Length; i++) sum += ps[i].Pos.X + ps[i].Pos.Y + ps[i].Pos.Z;
        return sum;
    }

    // pairwise attraction: an O(n^2) loop calling several small methods per pair
    static float Attract(Vec3[] pos, Vec3[] acc, int n) {
        for (int i = 0; i < n; i++) acc[i] = new Vec3(0, 0, 0);
        for (int i = 0; i < n; i++)
            for (int j = 0; j < n; j++) {
                if (i == j) continue;
                Vec3 d = pos[j] - pos[i];
                float r2 = Vec3.Dot(d, d) + 0.01f;
                acc[i] = acc[i] + d * (1f / (r2 * (float)Math.Sqrt(r2)));
            }
        float s = 0f; for (int i = 0; i < n; i++) s += acc[i].X + acc[i].Y + acc[i].Z;
        return s;
    }

    static void Report(string name, Stopwatch sw, float v) { Console.WriteLine(name + ": " + sw.ElapsedMilliseconds + " ms  checksum=" + ((double)v).ToString()); }

    static void Main() {
        Particle[] ps = new Particle[1000];
        for (int i = 0; i < ps.Length; i++) { ps[i].Pos = new Vec3(i * 0.01f, 10f + i % 7, i * 0.02f); ps[i].Vel = new Vec3(1f, 0f, -0.5f); ps[i].Mass = 1f; }
        Vec3[] pos = new Vec3[120], acc = new Vec3[120];
        for (int i = 0; i < pos.Length; i++) pos[i] = new Vec3((i * 37 % 101) * 0.1f, (i * 53 % 97) * 0.1f, (i * 71 % 89) * 0.1f);
        for (int round = 0; round < 3; round++) {
            Stopwatch sw = Stopwatch.StartNew(); float v = VecOps(200000); Report("vec ops   ", sw, v);
            sw = Stopwatch.StartNew(); v = Particles(ps, 100); Report("particles ", sw, v);
            sw = Stopwatch.StartNew(); v = 0f; for (int k = 0; k < 4; k++) v += Attract(pos, acc, pos.Length); Report("attract   ", sw, v);
        }
    }
}
