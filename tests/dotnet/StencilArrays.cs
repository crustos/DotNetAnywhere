using System;
using System.Threading;

// Array access in native blocks (see StencilBlocks.cs): loops over float[] and int[], a bool[] sieve, struct arrays (ldelema +
// ldfld/stfld), .Length in the loop condition, and the failures: an index out of range in the middle of a loop must throw at that
// iteration with the earlier writes visible and none after, a null array and a negative index likewise. Runs far past a time
// slice (the block gives up the processor and is entered again), and three threads on arrays at once.
// Compared with Mono line for line; the suite also runs it with DNA_NO_STENCILS=1.
struct Pt { public float X, Y; public int Id; }

class Shared { public int Done1, Done2; public float R1, R2; }

class Program {
    static uint h;
    static void Mix(uint v) { h = (h ^ v) * 16777619u; }
    static unsafe void MixF(float f) { if (f != f) { Mix(0x7fc00000u); } else { Mix(*(uint*)&f); } }

    static float Sum(float[] a) { float s = 0f; for (int i = 0; i < a.Length; i++) s = s + a[i]; return s; }
    static void Saxpy(float[] y, float[] x, float k, int n) { for (int i = 0; i < n; i++) y[i] = y[i] + x[i] * k; }
    static int SumInt(int[] a) { int s = 0; for (int i = 0; i < a.Length; i++) s = s + (a[i] ^ i); return s; }
    static void Prefix(int[] a) { for (int i = 1; i < a.Length; i++) a[i] = a[i] + a[i - 1]; }
    static int Sieve(bool[] composite, int n) {
        int count = 0;
        for (int i = 2; i < n; i++) {
            if (!composite[i]) { count++; for (int j = i + i; j < n; j += i) composite[j] = true; }
        }
        return count;
    }
    static void Mark(byte[] b, int n) { for (int i = 0; i < n; i++) { b[i] = 3; if (i > 0) b[i] = b[i - 1]; } }
    // byte[] elements are 1 byte and bool[] elements 4 in this runtime, and ldelem.u1 / stelem.i1 are used on both: a wrong
    // stride shows as neighbours being overwritten or read
    static int BytePattern(byte[] b) {
        for (int i = 0; i < b.Length; i += 2) b[i] = 1;                       // every other byte
        int s = 0;
        for (int i = 0; i < b.Length; i++) s = s * 3 + b[i];
        for (int i = 1; i < b.Length; i += 2) b[i] = b[i - 1];                // each odd one copies the one before
        for (int i = 0; i < b.Length; i++) s = s * 5 + b[i];
        return s;
    }
    static int BoolPattern(bool[] f) {
        for (int i = 0; i < f.Length; i += 3) f[i] = true;
        int s = 0;
        for (int i = 0; i < f.Length; i++) { if (f[i]) s = s * 7 + i; }
        for (int i = 1; i < f.Length; i += 3) f[i] = f[i - 1];
        for (int i = 0; i < f.Length; i++) { if (f[i]) s = s * 11 + i; }
        return s;
    }
    // conv.u1 / conv.u2 on the way into a byte[] / ushort[] (a mask), and conv.i4 after .Length
    static int Narrowing(byte[] b, ushort[] u, int[] src) {
        for (int i = 0; i < b.Length; i++) { b[i] = (byte)(i * 7 + src[i % src.Length]); }
        for (int i = 0; i < u.Length; i++) { u[i] = (ushort)(i * 1000 + src[i % src.Length]); }
        int s = 0;
        for (int i = 0; i < b.Length; i++) s = s * 3 + b[i];
        for (int i = 0; i < u.Length; i++) s = s * 5 + u[i];
        // narrowing that stays in int locals, so that the whole loop is one native block: conv.u2, conv.i1, conv.i2, conv.u1
        for (int i = 0; i < 40; i++) {
            int w = (ushort)(i * 40000 + src[i % src.Length]);
            int x = (sbyte)(i * 37 + w);
            int y = (short)(i * 30011 + x);
            int z = (byte)(y + i);
            s = s * 7 + w + x + y + z;
        }
        return s;
    }
    static float Integrate(Pt[] ps, float dt) {
        float s = 0f;
        for (int i = 0; i < ps.Length; i++) { ps[i].X = ps[i].X + ps[i].Y * dt; s = s + ps[i].X; }
        return s;
    }
    static int SumIds(Pt[] ps) { int s = 0; for (int i = 0; i < ps.Length; i++) s += ps[i].Id; return s; }
    static int FaultMid(int[] a, int n) { int s = 0; for (int i = 0; i < n; i++) { a[i] = i + 1; s += a[i]; } return s; }
    static float Below(float[] a, int from) { float s = 0f; for (int i = from; i < from + 4; i++) s = s + a[i - 3]; return s; }

    static string Try(Func<float> f) {
        try { return "ok " + f(); }
        catch (IndexOutOfRangeException) { return "IndexOutOfRange"; }
        catch (NullReferenceException) { return "NullReference"; }
    }

    static float[] Make(int n, int seed) { float[] a = new float[n]; for (int i = 0; i < n; i++) a[i] = (i * 7 + seed) * 0.25f - 3f; return a; }

    static Shared S = new Shared();
    static void T1() { S.R1 = Sum(Make(20000, 1)); S.Done1 = 1; }
    static void T2() { S.R2 = Sum(Make(20000, 2)); S.Done2 = 1; }

    public static void Main() {
        h = 2166136261u;
        for (int n = 0; n < 40; n++) {
            float[] a = Make(n, n), b = Make(n, 3);
            MixF(Sum(a)); Saxpy(a, b, 0.5f, n); MixF(Sum(a));
            int[] c = new int[n]; for (int i = 0; i < n; i++) c[i] = i * i - 5;
            Mix((uint)SumInt(c)); Prefix(c); Mix((uint)SumInt(c));
        }
        Console.WriteLine("small arrays " + h); h = 2166136261u;

        // far more iterations than one time slice
        float[] big = Make(50000, 5);
        Console.WriteLine("Sum(50000) " + Sum(big));
        int[] bigi = new int[40000]; for (int i = 0; i < bigi.Length; i++) bigi[i] = i & 255;
        Console.WriteLine("SumInt(40000) " + SumInt(bigi));
        Prefix(bigi); Console.WriteLine("Prefix last " + bigi[bigi.Length - 1] + " mid " + bigi[20000]);
        Console.WriteLine("Sieve(30000) " + Sieve(new bool[30000], 30000));
        byte[] bytes = new byte[500]; bytes[0] = 9; Mark(bytes, 500); Console.WriteLine("Mark " + bytes[0] + " " + bytes[499]);

        Console.WriteLine("BytePattern " + BytePattern(new byte[61]) + " " + BytePattern(new byte[8]) + "  BoolPattern " + BoolPattern(new bool[50]) + " " + BoolPattern(new bool[7]));
        Console.WriteLine("Narrowing " + Narrowing(new byte[70], new ushort[40], new int[] { 1, 300, -5, 65535, 70000, int.MaxValue }) + " " + Narrowing(new byte[3], new ushort[2], new int[] { -1 }));
        Pt[] ps = new Pt[300];
        for (int i = 0; i < ps.Length; i++) { ps[i].X = i; ps[i].Y = 0.5f - i * 0.01f; ps[i].Id = i * 3; }
        float total = 0f; for (int step = 0; step < 10; step++) total = Integrate(ps, 0.1f);
        Console.WriteLine("Integrate " + total + " ids " + SumIds(ps) + " ps[7].X " + ps[7].X + " ps[299].X " + ps[299].X);

        // failures
        int[] data = new int[6];
        Console.WriteLine("fault mid-loop: " + Try(() => FaultMid(data, 10)) + "  data=" + data[0] + "," + data[1] + "," + data[2] + "," + data[3] + "," + data[4] + "," + data[5]);
        int[] data2 = new int[6];
        Console.WriteLine("no fault: " + Try(() => FaultMid(data2, 6)));
        Console.WriteLine("null array: " + Try(() => Sum(null)) + " " + Try(() => { Saxpy(null, new float[3], 1f, 3); return 0f; }) + " " + Try(() => { Saxpy(new float[3], null, 1f, 3); return 0f; }));
        float[] four = Make(8, 1);
        Console.WriteLine("negative index: " + Try(() => Below(four, 2)) + " " + Try(() => Below(four, 3)) + " " + Try(() => Below(four, 9)));
        Pt[] short1 = new Pt[2];
        Console.WriteLine("struct array fault: " + Try(() => { SumIds(short1); return Integrate(short1, 1f); }) + " " + Try(() => { Pt[] n = null; return SumIds(n); }));
        float[] x3 = new float[3], y3 = new float[3]; x3[0] = 1; x3[1] = 2; x3[2] = 3;
        Console.WriteLine("Saxpy overruns y: " + Try(() => { Saxpy(y3, x3, 2f, 5); return 0f; }) + " y=" + y3[0] + "," + y3[1] + "," + y3[2]);

        // three threads on arrays at once
        float e1 = Sum(Make(20000, 1)), e2 = Sum(Make(20000, 2));
        new Thread(new ThreadStart(T1)).Start();
        new Thread(new ThreadStart(T2)).Start();
        float mine = Sum(Make(20000, 1));
        while (S.Done1 == 0 || S.Done2 == 0) { Thread.Sleep(1); }
        Console.WriteLine("three threads, arrays: " + (S.R1 == e1) + " " + (S.R2 == e2) + " " + (mine == e1));
    }
}
