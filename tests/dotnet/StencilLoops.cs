using System;
using System.Threading;

// Loops compiled to native blocks (see StencilBlocks.cs and tools/gen_stencils.py): counted, while and do-while loops, loops
// with if/else, break, continue and an early return, nested loops, loops that run far past one time slice (the block
// gives up the processor and is later entered again where the loop's back-edge goes), a null reference in the middle of a
// loop, a spin-wait that only ends if another thread gets to run, and two threads running the same loop at once.
// Compared with Mono line for line; the suite also runs it with DNA_NO_STENCILS=1.
class Shared { public int Flag; public int Spin; public int Count; public int Done1, Done2, R1, R2; public float F; }
class Node { public int Count; }

class Program {
    static uint h;
    static void Mix(uint v) { h = (h ^ v) * 16777619u; }
    static unsafe void MixF(float f) { if (f != f) { Mix(0x7fc00000u); } else { Mix(*(uint*)&f); } }

    static int SumInt(int n) { int s = 0; for (int i = 0; i < n; i++) { s = s + (i ^ (i >> 2)); } return s; }
    static float FloatLoop(int n) { float a = 1f, b = 0.5f; for (int i = 0; i < n; i++) { a = a * 0.9999f + b; b = b * 0.9999f - 0.0001f * a; } return a + b; }
    static int WhileLoop(int n) { int i = 0, s = 0; while (i < n) { s += i * 3; i++; } return s; }
    static int DoWhile(int n) { int i = 0, s = 0; do { s += i; i++; } while (i < n); return s; }
    static int Nested(int n) { int s = 0; for (int i = 0; i < n; i++) for (int j = 0; j < n; j++) s += (i * j) & 7; return s; }
    static int IfElse(int n) { int s = 0; for (int i = 0; i < n; i++) { if ((i & 1) == 0) s += i; else s -= 3; if (s > 100000) s = 0; } return s; }
    static int BreakContinue(int n) {
        int s = 0;
        for (int i = 0; i < n; i++) {
            if ((i & 7) == 3) continue;
            if (s > 3000000) break;
            s += i;
        }
        return s;
    }
    static int EarlyReturn(int n) { for (int i = 0; i < n; i++) { if (i * i > 5000) return i; } return -1; }
    // two long loops in one frame, split into two blocks by the call between them: a block that gave up the processor must
    // not leave the next block in this frame starting somewhere in the middle
    static int TwoLoops(int n, int m) {
        int a = 0, b = 0;
        for (int i = 0; i < n; i++) { a = a + (i & 15); }
        Mix((uint)a);
        for (int j = 0; j < m; j++) { b = b + (j >> 1) + 1; }
        return a * 31 + b;
    }
    // unsigned shift right and bitwise or (and conv.u4): a rotate-and-mix loop on a uint
    static uint RotateLoop(uint x, int n) {
        for (int i = 0; i < n; i++) { x = (x >> 1) | (x << 31); x = x ^ (uint)i; x = x | 1u; }
        return x;
    }
    static int ZeroTrips(int n) { int s = 7; for (int i = 0; i < n; i++) { s += 1000; } return s; }
    static int Heavy(int seed, int n) { int a = seed, b = 1; for (int i = 0; i < n; i++) { a = a * 3 + b; b = b ^ (a >> 3); } return a + b; }
    // a loop whose condition is a field: the spin-wait
    static void Spinner(Shared sh) { int spins = 0; while (sh.Flag == 0) { spins = spins + 1; } sh.Spin = spins; }
    // faults in the middle of a loop, with work done before and a field being updated each time round
    static int NullInLoop(Node node, Shared good, int n) {
        int s = 0;
        for (int i = 0; i < n; i++) { s += i; good.Count++; if (i == 50) s += node.Count; }
        return s;
    }

    static Shared S = new Shared();
    static void Worker() { int x = 0; for (int i = 0; i < 300; i++) x += i; S.F = x; S.Flag = 1; }
    static void Heavy1() { S.R1 = Heavy(5, 20000); S.Done1 = 1; }
    static void Heavy2() { S.R2 = Heavy(9, 20000); S.Done2 = 1; }

    public static void Main() {
        h = 2166136261u;
        for (int n = 0; n < 40; n++) { Mix((uint)SumInt(n)); MixF(FloatLoop(n)); Mix((uint)WhileLoop(n)); Mix((uint)Nested(n % 9)); Mix((uint)IfElse(n)); }
        Console.WriteLine("small trip counts " + h); h = 2166136261u;
        // far more iterations than one time slice (100 back-edges): the block gives up the processor many times
        Console.WriteLine("SumInt " + SumInt(50000) + " Float " + FloatLoop(20000));
        Console.WriteLine("While " + WhileLoop(30000) + " DoWhile " + DoWhile(30000) + " DoWhile(1) " + DoWhile(1));
        Console.WriteLine("Nested " + Nested(300) + " IfElse " + IfElse(40000));
        Console.WriteLine("BreakContinue " + BreakContinue(60000) + " EarlyReturn " + EarlyReturn(1000) + " " + EarlyReturn(10));
        Console.WriteLine("TwoLoops " + TwoLoops(5000, 4000) + " " + TwoLoops(3, 3) + " zero trips in the second: " + TwoLoops(5000, 0) + " " + TwoLoops(0, 7));
        Console.WriteLine("RotateLoop " + RotateLoop(0x80000001u, 50) + " " + RotateLoop(12345u, 3000) + " " + RotateLoop(0u, 0));
        Console.WriteLine("ZeroTrips " + ZeroTrips(0) + " " + ZeroTrips(-5) + " " + ZeroTrips(3));

        Shared good = new Shared();
        try { NullInLoop(null, good, 200); Console.WriteLine("no exception"); }
        catch (NullReferenceException) { Console.WriteLine("null in a loop: NullReferenceException after " + good.Count + " iterations"); }
        good.Count = 0;
        Console.WriteLine("same loop, a real object: " + NullInLoop(new Node(), good, 200) + " count " + good.Count);

        // a spin-wait ends only if another thread gets to run
        S.Flag = 0;
        new Thread(new ThreadStart(Worker)).Start();
        Spinner(S);
        Console.WriteLine("spin-wait ended, the other thread ran: " + (S.Flag == 1) + ", it spun at least once: " + (S.Spin >= 0) + ", F=" + S.F);

        // two threads in the same loop at once, and this one too: each must get its own answer
        int expect1 = Heavy(5, 20000), expect2 = Heavy(9, 20000);
        new Thread(new ThreadStart(Heavy1)).Start();
        new Thread(new ThreadStart(Heavy2)).Start();
        int mine = Heavy(5, 20000);
        while (S.Done1 == 0 || S.Done2 == 0) { Thread.Sleep(1); }
        Console.WriteLine("three threads, one loop: " + (S.R1 == expect1) + " " + (S.R2 == expect2) + " " + (mine == expect1));
    }
}
