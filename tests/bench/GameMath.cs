// Game / vector-math kernels for comparing execution engines. Each kernel prints a checksum (so engines can be
// checked against each other and nothing is optimised away) and its time in ms.
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

class GameMath {
    static void Report(string name, Stopwatch sw, float checksum) {
        Console.WriteLine(name + ": " + sw.ElapsedMilliseconds + " ms  checksum=" + checksum.ToString());
    }

    // struct math through operators: dot, cross, normalize
    static void VecOps(int n) {
        Stopwatch sw = Stopwatch.StartNew();
        Vec3 acc = new Vec3(0, 0, 0), a = new Vec3(1, 2, 3), b = new Vec3(0.5f, -1f, 2f);
        for (int i = 0; i < n; i++) {
            Vec3 c = Vec3.Cross(a, b);
            acc = acc + c.Normalized() * 0.001f;
            a = a + b * 0.0001f;
        }
        Report("vec3 ops      ", sw, acc.X + acc.Y + acc.Z + Vec3.Dot(a, b));
    }

    // structure-of-arrays particle integration with a floor bounce: the typical inner loop
    static void Particles(int count, int steps) {
        float[] px = new float[count], py = new float[count], pz = new float[count];
        float[] vx = new float[count], vy = new float[count], vz = new float[count];
        for (int i = 0; i < count; i++) { px[i] = i * 0.01f; py[i] = 10f + (i % 7); pz[i] = i * 0.02f; vx[i] = 1f; vy[i] = 0f; vz[i] = -0.5f; }
        Stopwatch sw = Stopwatch.StartNew();
        const float dt = 0.016f, g = -9.8f;
        for (int s = 0; s < steps; s++) {
            for (int i = 0; i < count; i++) {
                vy[i] += g * dt;
                px[i] += vx[i] * dt; py[i] += vy[i] * dt; pz[i] += vz[i] * dt;
                if (py[i] < 0f) { py[i] = -py[i]; vy[i] = -vy[i] * 0.8f; }
            }
        }
        float sum = 0; for (int i = 0; i < count; i++) sum += px[i] + py[i] + pz[i];
        Report("particles     ", sw, sum);
    }

    // 4x4 matrix multiply on float[16]
    static void Mat4(int n) {
        float[] a = new float[16], b = new float[16], r = new float[16];
        for (int i = 0; i < 16; i++) { a[i] = (i % 5) * 0.1f + 0.5f; b[i] = (i % 3) * 0.2f + 0.25f; }
        float trace = 0;
        Stopwatch sw = Stopwatch.StartNew();
        for (int k = 0; k < n; k++) {
            a[0] += 0.0001f;      // perturb the input so the multiply is not loop-invariant
            for (int row = 0; row < 4; row++)
                for (int col = 0; col < 4; col++) {
                    float s = 0;
                    for (int j = 0; j < 4; j++) s += a[row * 4 + j] * b[j * 4 + col];
                    r[row * 4 + col] = s;
                }
            trace += r[0] + r[5] + r[10] + r[15];
        }
        Report("mat4 multiply ", sw, trace);
    }

    // scalar int loop with branches (collision-grid style integer work)
    static void IntGrid(int n) {
        Stopwatch sw = Stopwatch.StartNew();
        int h = 17, hits = 0;
        for (int i = 0; i < n; i++) {
            h = h * 1103515245 + 12345;
            int cx = (h >> 8) & 63, cy = (h >> 16) & 63;
            if (((cx * 31 + cy * 17) & 7) == 0) hits++;
        }
        Report("int grid hash ", sw, hits);
    }

    static void Main() {
        VecOps(300000);
        Particles(1000, 200);
        Mat4(30000);
        IntGrid(2000000);
    }
}
