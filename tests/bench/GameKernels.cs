// Game / vector-math kernels written the way the wasm JIT can take them: loops in static methods over arrays (structure-of-arrays, as
// game code tends to do for the hot paths). Each prints a checksum, so the engines can be compared for the same answer, and its time.
using System;
using System.Diagnostics;

class GameKernels {
    // integrate particles under gravity with a floor bounce
    static void Particles(float[] px, float[] py, float[] pz, float[] vx, float[] vy, float[] vz, int count, int steps) {
        const float dt = 0.016f, g = -9.8f;
        for (int s = 0; s < steps; s++) {
            for (int i = 0; i < count; i++) {
                vy[i] += g * dt;
                px[i] += vx[i] * dt; py[i] += vy[i] * dt; pz[i] += vz[i] * dt;
                if (py[i] < 0f) { py[i] = -py[i]; vy[i] = -vy[i] * 0.8f; }
            }
        }
    }

    // many 4x4 matrix multiplies (row-major float[16]), the result feeding the next so nothing is loop-invariant
    static float Mat4(float[] a, float[] b, float[] r, int n) {
        float trace = 0f;
        for (int k = 0; k < n; k++) {
            a[0] += 0.0001f;
            for (int row = 0; row < 4; row++)
                for (int col = 0; col < 4; col++) {
                    float s = 0f;
                    for (int j = 0; j < 4; j++) s += a[row * 4 + j] * b[j * 4 + col];
                    r[row * 4 + col] = s;
                }
            trace += r[0] + r[5] + r[10] + r[15];
        }
        return trace;
    }

    // dot products and an axpy over long float arrays (the core of most vector code)
    static float DotAxpy(float[] x, float[] y, int n, int reps) {
        float acc = 0f;
        for (int r = 0; r < reps; r++) {
            float d = 0f;
            for (int i = 0; i < n; i++) d += x[i] * y[i];
            for (int i = 0; i < n; i++) y[i] += 0.001f * x[i];
            acc += d * 0.0001f;
        }
        return acc;
    }

    // integer hashing into a grid, counting cells: collision-grid style integer work with / and %
    static int IntGrid(int n) {
        int h = 17, hits = 0;
        for (int i = 0; i < n; i++) {
            h = h * 1103515245 + 12345;
            int cx = (h >> 8) & 63, cy = (h >> 16) & 63;
            if (((cx * 31 + cy * 17) % 8) == 0) hits++;
            hits += (cx / 7) & 1;
        }
        return hits;
    }

    // 1D convolution / box filter over a float signal
    static float Blur(float[] src, float[] dst, int n, int passes) {
        for (int p = 0; p < passes; p++) {
            for (int i = 1; i < n - 1; i++) dst[i] = (src[i - 1] + src[i] + src[i + 1]) * 0.33333334f;
            for (int i = 1; i < n - 1; i++) src[i] = dst[i];
        }
        float s = 0f; for (int i = 0; i < n; i++) s += src[i];
        return s;
    }

    static void Report(string name, Stopwatch sw, string checksum) { Console.WriteLine(name + ": " + sw.ElapsedMilliseconds + " ms  checksum=" + checksum); }

    static void Main() {
        int n = 2000;
        float[] px = new float[n], py = new float[n], pz = new float[n], vx = new float[n], vy = new float[n], vz = new float[n];
        for (int i = 0; i < n; i++) { px[i] = i * 0.01f; py[i] = 10f + (i % 7); pz[i] = i * 0.02f; vx[i] = 1f; vz[i] = -0.5f; }
        Stopwatch sw = Stopwatch.StartNew();
        Particles(px, py, pz, vx, vy, vz, n, 400);
        float sum = 0f; for (int i = 0; i < n; i++) sum += px[i] + py[i] + pz[i];
        Report("particles   ", sw, sum.ToString());

        float[] a = new float[16], b = new float[16], r = new float[16];
        for (int i = 0; i < 16; i++) { a[i] = (i % 5) * 0.1f + 0.5f; b[i] = (i % 3) * 0.2f + 0.25f; }
        sw = Stopwatch.StartNew();
        float t = Mat4(a, b, r, 100000);
        Report("mat4 multiply", sw, t.ToString());

        float[] x = new float[4096], y = new float[4096];
        for (int i = 0; i < 4096; i++) { x[i] = (i % 17) * 0.1f; y[i] = (i % 13) * 0.2f; }
        sw = Stopwatch.StartNew();
        float da = DotAxpy(x, y, 4096, 200);
        Report("dot+axpy    ", sw, da.ToString());

        sw = Stopwatch.StartNew();
        int hits = IntGrid(3000000);
        Report("int grid    ", sw, hits.ToString());

        float[] sig = new float[8192], tmp = new float[8192];
        for (int i = 0; i < 8192; i++) sig[i] = (i * 7919 % 1000) * 0.001f;
        sw = Stopwatch.StartNew();
        float bs = Blur(sig, tmp, 8192, 50);
        Report("blur        ", sw, bs.ToString());
    }
}
