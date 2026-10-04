using System;
using System.Threading;

// Islands: instructions that a native block gives to the interpreter (calls, throw, newobj, ldstr, boxing, casts, static fields) and takes
// back after, so that a loop or a method with a call in the middle is still one block. This checks what has to be the same: calls in the
// middle of hot loops, a throw from the middle of a method (caught by the caller, which has the handler), objects, strings, boxing and
// casts, a static field whose type initialiser runs inside an island, allocation (and so collection) while a block waits, a recursion
// that goes through blocks, and threads that give up the processor in blocks with islands. Compared with Mono line for line; the suite
// also runs it with DNA_NO_ISLANDS=1.
class Log { public static int order; }                                         // (no initialiser: touching it runs nothing)
class Statics {
    public static int x; public static long y = 77;
    static Statics() { Log.order = Log.order * 10 + 7; x = 40; }                 // an explicit static constructor: it runs at the first access, on any runtime
}
class Other { public static string s; static Other() { Log.order = Log.order * 10 + 3; s = "o9"; } }
class Thing { public int a; public long b; public object o; public Thing(int a) { this.a = a; b = a * 3L; } public int Twice() { return a * 2; } }
struct Pair { public int A; public long B; public Pair(int a, long b) { A = a; B = b; } }
class Program {
    static uint h;
    static void Mix(uint v) { h = (h ^ v) * 16777619u; }
    static void MixL(long v) { Mix((uint)v); Mix((uint)(v >> 32)); }
    static void Group(string name) { Console.WriteLine(name + " " + h); h = 2166136261u; }

    static int calls;
    static int Leaf(int x) { calls++; return x * 2 + 1; }                         // touches a static: not a leaf for inlining, so an island
    static long LeafL(long x, int k) { calls += k; return x * 3 - k; }
    static int Loop1(int n) { int s = 0; for (int i = 0; i < n; i++) { s += i * 3; s ^= Leaf(i); s += (i << 1) ^ 5; } return s; }
    static long Loop2(int n) { long s = 1; for (int i = 1; i <= n; i++) { s = s * 31 + LeafL(s, i & 3); if ((s & 1) == 0) { s += Leaf(i); } s ^= i; } return s; }
    // The loop's first instructions are in the same short region as the setup before it, and the call ends that region: the back-edge is
    // then outside the block, and must not be allowed to jump into the middle of it (it used to land on the block's start, and run forever)
    static long Truncated(int n) { long churn = 0; for (int i = 0; i < n; i++) { churn += Leaf(300); } return churn; }
    static long Truncated2(int n) { long churn = 5; int i = 0; while (i < n) { churn += LeafL(churn, 2) & 1023; i++; } return churn; }
    static int Straight(int a, int b) { int r = a * 3 + b; r = r ^ Leaf(r); r = r * 5 + Leaf(a) - b; r += Leaf(b) << 2; return r + a; }

    static int Thrower(int a, int b) {
        int r = a * 3 + b;
        if (r > 1000) { throw new InvalidOperationException("big" + r); }
        r = r * 2 + a;
        return r + Leaf(r);
    }
    static int Caller(int n) {
        int ok = 0, bad = 0;
        for (int i = 0; i < n; i++) { try { ok += Thrower(i * 37, i); } catch (InvalidOperationException e) { bad += e.Message.Length; } }
        return ok * 1000 + bad;
    }

    // a try body with enough around the call to be a block of its own: the call is an island only if the method has no handler, and if it
    // did have one the throw would come from a stub outside the try range and not be caught
    static int InsideTry(int n) {
        int ok = 0, bad = 0, w = 1;
        for (int i = 0; i < n; i++) {
            try {
                int a = i * 3 + 1; a ^= i << 2; a += i * 7; a -= i; a = a * 5 + 3; a ^= a >> 3; a += i * 11; a = a * 3 - 1;
                ok += Thrower(a, i);
                w = w * 3 + a; w ^= i; w += a * 2; w = w * 7 - i; w ^= w >> 2; w += i * 5; w = w * 3 + 1; w -= a;
            } catch (InvalidOperationException e) { bad += e.Message.Length + (w & 7); }
        }
        return ok * 1000 + bad + w;
    }
    static object Boxer(int i) { object o = i; object p = (long)i << 3; object q = i * 0.5; object r = new Pair(i, i * 5L); return (i & 1) == 0 ? o : ((i & 2) == 0 ? p : ((i & 4) == 0 ? q : r)); }
    static int Cast(object o) { int r = 0; if (o is int) r += (int)o; if (o is long) r += (int)(long)o; if (o is double) r += (int)(double)o; if (o is Pair) r += ((Pair)o).A; if (o is string) r += ((string)o).Length; if (o is Thing) r += ((Thing)o).Twice(); return r; }
    static int Objects(int n) { int s = 0; for (int i = 0; i < n; i++) { Thing t = new Thing(i); t.o = (i % 3 == 0) ? (object)"s" : (object)t; s += t.a + (int)t.b + (t.o is string ? 1 : 2) + t.Twice(); } return s; }
    static int Strings(int n) { int s = 0; for (int i = 0; i < n; i++) { string t = "x" + i; s += t.Length + ("k" + (i & 7)).Length; } return s; }
    static int StaticField(int n) { int s = 0; for (int i = 0; i < n; i++) { s += Statics.x + i; Statics.y += i; } return s + (int)Statics.y; }
    static int Rec(int n) { if (n <= 0) return Leaf(0); int s = n * 3; s += Rec(n - 1); s ^= Leaf(n); return s; }

    static int shared;
    static void Worker1() { int s = 0; for (int i = 0; i < 60000; i++) { s += Leaf(i) & 7; } shared += s; }
    static void Worker2() { long s = 0; for (int i = 0; i < 60000; i++) { s += LeafL(i, 1) & 15; } shared += (int)s; }
    static int done;
    static void Run1() { Worker1(); done += 1; }
    static void Run2() { Worker2(); done += 10; }

    public static void Main() {
        h = 2166136261u;
        for (int n = 0; n < 12; n++) { Mix((uint)Loop1(n)); MixL(Loop2(n)); }
        Mix((uint)Loop1(20000)); MixL(Loop2(20000)); Mix((uint)calls);
        for (int i = -3; i < 40; i++) { Mix((uint)Straight(i, 7 - i)); }
        for (int n = 0; n < 6; n++) { MixL(Truncated(n)); MixL(Truncated2(n)); }
        Group("calls in the middle of loops");
        for (int n = 0; n < 40; n += 3) { Mix((uint)Caller(n)); Mix((uint)InsideTry(n)); }
        Mix((uint)InsideTry(200));
        Mix((uint)Caller(60));
        Group("throw from the middle of a method");
        for (int i = 0; i < 24; i++) { object o = Boxer(i); Mix((uint)Cast(o)); Mix((uint)Cast("str" + i)); Mix((uint)Cast(new Thing(i))); }
        Mix((uint)Objects(500)); Mix((uint)Strings(300));
        Group("objects, strings, boxing, casts");
        Log.order = 0; Mix((uint)StaticField(3)); Mix((uint)Log.order); Mix((uint)StaticField(50));
        Mix((uint)(Other.s.Length + Log.order));
        Group("static fields and type initialisers");
        for (int n = 0; n < 15; n += 2) { Mix((uint)Rec(n)); }
        Mix((uint)Rec(25));
        long churn = 0; for (int i = 0; i < 30; i++) { churn += Objects(300) + Strings(100); GC.Collect(); }
        MixL(churn);
        Group("recursion and allocation");
        new Thread(new ThreadStart(Run1)).Start();
        new Thread(new ThreadStart(Run2)).Start();
        int spin = 0; while (done != 11) { Thread.Sleep(1); spin++; }
        Mix((uint)shared); Mix(done == 11 ? 1u : 0u);
        Group("threads");
    }
}
