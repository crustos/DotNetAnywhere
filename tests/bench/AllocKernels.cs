// Allocation-heavy code: objects with constructors, linked structures and arrays made in loops.
using System;
using System.Diagnostics;

class Vec { public float X, Y, Z; public Vec(float x, float y, float z) { X = x; Y = y; Z = z; } public Vec Add(Vec o) { return new Vec(X + o.X, Y + o.Y, Z + o.Z); } public Vec Scale(float s) { return new Vec(X * s, Y * s, Z * s); } }
class Node { public int V; public Node Next; public Node(int v, Node n) { V = v; Next = n; } }

class AllocKernels {
    // a new object per operation, as code written with a Vec class does
    static float VecChain(int n) {
        Vec acc = new Vec(0f, 0f, 0f), step = new Vec(1f, 0.5f, 0.25f);
        for (int i = 0; i < n; i++) acc = acc.Add(step.Scale(0.001f * (i & 7)));
        return acc.X + acc.Y + acc.Z;
    }
    // lists built and summed
    static long Lists(int rounds, int len) {
        long total = 0;
        for (int r = 0; r < rounds; r++) {
            Node h = null;
            for (int i = 0; i < len; i++) h = new Node(i + r, h);
            for (Node p = h; p != null; p = p.Next) total += p.V;
        }
        return total;
    }
    // arrays allocated per iteration
    static long Arrays(int n) {
        long total = 0;
        for (int i = 0; i < n; i++) { int[] a = new int[16 + (i & 15)]; for (int j = 0; j < a.Length; j++) a[j] = j * i; total += a[a.Length - 1]; }
        return total;
    }
    static void Report(string name, Stopwatch sw, string v) { Console.WriteLine(name + ": " + sw.ElapsedMilliseconds + " ms  checksum=" + v); }
    static void Main() {
        for (int round = 0; round < 3; round++) {
            Stopwatch sw = Stopwatch.StartNew(); float f = VecChain(100000); Report("vec objects", sw, ((double)f).ToString());
            sw = Stopwatch.StartNew(); long l = Lists(20, 5000); Report("lists      ", sw, l.ToString());
            sw = Stopwatch.StartNew(); l = Arrays(50000); Report("arrays     ", sw, l.ToString());
        }
    }
}
