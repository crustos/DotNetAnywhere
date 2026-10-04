using System;

// Delegate invocation: one target (the usual case, whose arguments are used where they are), several targets (whose arguments are kept for
// each), static and instance targets, closures, arguments of different sizes including a struct, with and without a result, adding and
// removing targets, equality of delegates, boxing an IntPtr, a null delegate, delegates that call delegates, and an exception thrown from a target. Compared with Mono line for line.
struct P { public int A; public long B; public double C; }
delegate int D0();
delegate int D1(int a);
delegate long D2(long a, int b);
delegate double D3(double a, float b, long c);
delegate int DP(P p, int k);
delegate void DV(int a);
delegate void DRef(ref int a, out long b, int c);
class Holder {
    public int field; public long total;
    public int Add(int a) { field += a; return field; }
    public long Mul(long a, int b) { total += a * b; return total; }
    public void Note(int a) { total = total * 31 + a; }
    public int Const() { return field * 2 + 1; }
}
class Program {
    static uint h;
    static void Mix(uint v) { h = (h ^ v) * 16777619u; }
    static void MixL(long v) { Mix((uint)v); Mix((uint)(v >> 32)); }
    static void Group(string name) { Console.WriteLine(name + " " + h); h = 2166136261u; }

    static int S0() { return 41; }
    static int S1(int a) { return a * 2 + 1; }
    static int S1b(int a) { return a - 3; }
    static int S1c(int a) { return a ^ 0x2a; }
    static long S2(long a, int b) { return a * 3 + b; }
    static double S3(double a, float b, long c) { return a * b + c; }
    static int SP(P p, int k) { return p.A * k + (int)p.B + (int)p.C; }
    static long acc;
    static void SV(int a) { acc = acc * 7 + a; }
    static void SV2(int a) { acc = acc * 5 - a; }
    static void SRef(ref int a, out long b, int c) { a += c; b = (long)a << 33; }
    static int Thrower(int a) { if (a == 3) throw new InvalidOperationException("t"); return a + 1; }
    static int Nested(D1 d, int a) { return d(a) + d(a + 1); }

    public static void Main() {
        h = 2166136261u;
        Holder hd = new Holder();
        int cap = 10;
        D0 d0 = S0; D1 d1 = S1; D1 d1i = hd.Add; D1 dcl = x => x + cap; D2 d2 = S2; D2 d2i = hd.Mul; D3 d3 = S3; DP dp = SP;
        for (int i = 0; i < 40; i++) {
            cap = i;
            Mix((uint)(d0() + d1(i) + d1i(i) + dcl(i) + hd.Const()));
            MixL(d2(i * 1000003L, i) + d2i((long)i << 20, i + 1));
            MixL((long)(d3(i * 0.5, i / 3f, (long)i << 30) * 100));
            P p; p.A = i; p.B = (long)i * 5; p.C = i * 0.75; Mix((uint)dp(p, i + 2));
        }
        Group("single target");
        // several targets: all run, in order, and the last one's result is the result
        D1 multi = S1; multi += S1b; multi += hd.Add; multi += S1c;
        DV mv = SV; mv += SV2; mv += hd.Note; mv += SV;
        for (int i = 0; i < 30; i++) {
            Mix((uint)multi(i)); acc = 0; mv(i); mv(i + 1); MixL(acc); MixL(hd.total); Mix((uint)hd.field);
        }
        // removing targets (Delegate.Equals boxes an IntPtr, which had no handler: this crashed), and equality of delegates
        multi -= S1b; multi -= S1c;
        for (int i = 0; i < 10; i++) { Mix((uint)multi(i)); }
        D1 once = S1; once += S1; Mix((uint)once(5)); once -= S1; Mix((uint)once(6)); once -= S1; Mix(once == null ? 1u : 0u);
        D1 e1 = S1, e2 = S1, e3 = S1b, e4 = hd.Add, e5 = hd.Add; Holder other = new Holder(); D1 e6 = other.Add;
        Mix((e1.Equals(e2) ? 1u : 0u) + (e1.Equals(e3) ? 2u : 0u) + (e4.Equals(e5) ? 4u : 0u) + (e4.Equals(e6) ? 8u : 0u) + (e1.Equals(null) ? 16u : 0u) + (e1.Equals((object)5) ? 32u : 0u));
        Mix((uint)(e1.GetHashCode() == e2.GetHashCode() ? 1 : 0)); Mix((uint)(e4.GetHashCode() == e5.GetHashCode() ? 1 : 0));
        IntPtr ip = new IntPtr(12345), ip2 = new IntPtr(12345); object boxed = ip;
        Mix((ip.Equals(boxed) ? 1u : 0u) + (ip.Equals((object)ip2) ? 2u : 0u) + (ip.Equals((object)new IntPtr(7)) ? 4u : 0u) + (ip.Equals((object)7) ? 8u : 0u));
        D1 three = S1; three += S1b; three += hd.Add; for (int i = 0; i < 10; i++) { Mix((uint)three(i)); }
        Group("several targets");
        // ref and out arguments, a delegate passed down and invoked there, a null delegate, an exception from a target
        DRef dr = SRef; int ra = 5; long rb;
        for (int i = 0; i < 10; i++) { dr(ref ra, out rb, i); Mix((uint)ra); MixL(rb); }
        for (int i = 0; i < 8; i++) { Mix((uint)Nested(S1, i)); Mix((uint)Nested(hd.Add, i)); Mix((uint)Nested(dcl, i)); Mix((uint)Nested(multi, i)); }
        D1 nul = null; int caught = 0;
        for (int i = 0; i < 6; i++) {
            try { caught += nul(i); } catch (NullReferenceException) { caught += 1; }
            try { caught += Thrower(i) + new D1(Thrower)(i); } catch (InvalidOperationException e) { caught += 100 + e.Message.Length; }
        }
        Mix((uint)caught);
        Group("ref, nesting, null, exceptions");
        // many invocations of the same delegates from one frame (each one used to keep a copy of its arguments)
        long sum = 0;
        for (int i = 0; i < 20000; i++) { sum += d1(i & 255) + dcl(i & 15); sum += multi(i & 63); }
        MixL(sum);
        Group("a long loop");
    }
}
