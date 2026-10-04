using System;

// The ordinary call and return (the ones that are not inlined): frames of every shape. Argument areas of 0 to 80 bytes (the copy of the
// arguments has special cases for some sizes and a general one for the rest), structs by value as arguments and results, ref and out
// arguments, locals, virtual dispatch through several levels (including base. calls, abstract methods and new slots), a null receiver,
// recursion with many arguments, and exceptions thrown through many frames (finally blocks run, locals of each frame intact).
// Compared with Mono line for line.
struct V3 { public int X, Y, Z; public V3(int x, int y, int z) { X = x; Y = y; Z = z; } }
struct Big { public long A, B, C, D, E, F, G, H; }               // 64 bytes
struct Mixed { public byte B; public short S; public int I; public long L; public double D; }
interface IA { int F(int x); }
interface IB { int G(int x); }
interface IC : IA { int H(int x); }
interface IG<T> { T Get(T v); }
class Ca : IA { public int F(int x) { return x + 1; } }
class Cab : IA, IB { public int F(int x) { return x * 2; } public int G(int x) { return x - 1; } }
class Cabd : Cab { public new int F(int x) { return x * 3; } }                 // does not re-implement IA: IA.F is still Cab.F
class Cre : Cab, IA { public new int F(int x) { return x * 4; } }              // re-implements IA
class Cexp : IA, IB { int IA.F(int x) { return x + 100; } int IB.G(int x) { return x + 200; } public int F(int x) { return -x; } }
class Cc : IC { public int F(int x) { return x ^ 7; } public int H(int x) { return x + 7; } }
class Cg : IG<int> { public int Get(int v) { return v + 1; } }
class Cl : IG<long> { public long Get(long v) { return v * 3; } }      // (one class implementing both IG<int> and IG<long> crashes DNA: a known gap)
abstract class Animal {
    public int legs;
    public abstract int Sound(int x);
    public virtual int Speak(int a, long b) { return Sound(a) + (int)b + legs; }
    public int Plain(int a) { return a * 3 + legs; }
}
class Dog : Animal {
    public Dog() { legs = 4; }
    public override int Sound(int x) { return x + 100; }
    public override int Speak(int a, long b) { return base.Speak(a, b) * 2; }
}
class Puppy : Dog {
    public override int Sound(int x) { return base.Sound(x) - 1; }
    public new int Plain(int a) { return a * 5; }
}
class Bird : Animal {
    public Bird() { legs = 2; }
    public override int Sound(int x) { return x ^ 0x55; }
}
class Program {
    static uint h;
    static void Mix(uint v) { h = (h ^ v) * 16777619u; }
    static void MixL(long v) { Mix((uint)v); Mix((uint)(v >> 32)); }
    static void Group(string name) { Console.WriteLine(name + " " + h); h = 2166136261u; }

    static int A0() { return 7; }
    static int A1(int a) { return a + 1; }
    static int A2(int a, int b) { return a * 2 + b; }
    static int A3(int a, int b, int c) { return a * 3 + b * 2 + c; }
    static int A4(int a, int b, int c, int d) { return a + b * 2 + c * 3 + d * 4; }
    static int A5(int a, int b, int c, int d, int e) { return a + b * 2 + c * 3 + d * 4 + e * 5; }
    static int A7(int a, int b, int c, int d, int e, int f, int g) { return a + b * 2 + c * 3 + d * 4 + e * 5 + f * 6 + g * 7; }
    static int A13(int a, int b, int c, int d, int e, int f, int g, int h, int i, int j, int k, int l, int m) {
        return a + b * 2 + c * 3 + d * 4 + e * 5 + f * 6 + g * 7 + h * 8 + i * 9 + j * 10 + k * 11 + l * 12 + m * 13;
    }
    static long L2(long a, long b) { return a * 3 - b; }
    static double D3(double a, double b, double c) { return a * b + c; }
    static long Mix5(int a, long b, int c, double d, float e) { return a + b * 2 + c * 3 + (long)(d * 4) + (long)(e * 5); }
    static int Sv(V3 v) { return v.X + v.Y * 2 + v.Z * 3; }
    static int Sv2(V3 v, V3 w, int k) { return Sv(v) * k + Sv(w); }
    static long Sbig(Big b, int k) { return b.A + b.B * 2 + b.C * 3 + b.D * 4 + b.E * 5 + b.F * 6 + b.G * 7 + b.H * 8 + k; }
    static long Smix(Mixed m, int k) { return m.B + m.S * 2 + m.I * 3 + m.L * 4 + (long)m.D + k; }
    static V3 RetV(int a) { return new V3(a, a * 2, a * 3); }
    static Big RetBig(int a) { Big b = new Big(); b.A = a; b.H = a * 8L; b.D = -a; return b; }
    static Mixed RetMixed(int a) { Mixed m = new Mixed(); m.B = (byte)a; m.S = (short)(a * 3); m.I = a * 5; m.L = (long)a << 33; m.D = a / 4.0; return m; }
    static long RetL(int a) { return (long)a << 35 | (uint)a; }
    static double RetD(int a) { return a / 3.0; }
    static void Out(int a, out int x, out long y, ref double z) { x = a * 2; y = (long)a << 40; z = z * 2 + a; }
    static void Fill(int[] arr, int v) { for (int i = 0; i < arr.Length; i++) arr[i] = v + i; }
    static int Locals(int a) { int x = a, y = a * 2, z = a * 3; long w = a; double d = a * 0.5; int[] arr = new int[3]; Fill(arr, a); return x + y + z + (int)w + (int)d + arr[0] + arr[2]; }
    static int Rec(int n, long a, int b, int c, double d) { if (n == 0) return b + c + (int)a + (int)d; return Rec(n - 1, a + n, b ^ n, c + 1, d + 0.5) + n; }
    static int Even(int n) { return n == 0 ? 1 : Odd(n - 1); }
    static int Odd(int n) { return n == 0 ? 0 : Even(n - 1); }
    static int fin;
    static int Throwing(int depth, int mark) {
        int local = mark * 3;
        try { if (depth == 0) throw new InvalidOperationException("x" + mark); return Throwing(depth - 1, mark + 1) + local; }
        finally { fin = fin * 7 + local + depth; }
    }
    static int Catcher(int depth, int mark) { try { return Throwing(depth, mark); } catch (InvalidOperationException e) { return -1000 - e.Message.Length - mark; } }

    public static void Main() {
        h = 2166136261u;
        for (int i = -4; i < 20; i++) {
            Mix((uint)(A0() + A1(i) + A2(i, 3) + A3(i, 3, 5) + A4(i, 1, 2, 3) + A5(i, 1, 2, 3, 4) + A7(i, 1, 2, 3, 4, 5, 6)));
            Mix((uint)A13(i, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, i * 2));
            MixL(L2(i * 1000003L, 77) + Mix5(i, (long)i << 33, i + 1, i * 0.5, i / 3f)); MixL((long)(D3(i, 1.5, 0.25) * 1000));
        }
        Group("argument areas");
        for (int i = 0; i < 12; i++) {
            V3 v = new V3(i, i + 1, i + 2); Big b = RetBig(i); Mixed m = RetMixed(i);
            Mix((uint)(Sv(v) + Sv2(v, RetV(i * 2), i)));
            MixL(Sbig(b, i)); MixL(Smix(m, i)); MixL(Sbig(RetBig(i + 1), Sv(RetV(i)))); MixL(RetL(i)); MixL((long)(RetD(i) * 1000));
            Mix((uint)(RetV(i).Y + RetV(i + 1).Z)); MixL(RetBig(i).H + RetMixed(i).L);
        }
        Group("structs by value");
        double z = 1.5; long y; int x;
        for (int i = 0; i < 10; i++) { Out(i, out x, out y, ref z); Mix((uint)x); MixL(y); MixL((long)(z * 100)); Mix((uint)Locals(i)); }
        Group("ref, out, locals");
        Animal[] zoo = { new Dog(), new Puppy(), new Bird(), new Dog(), new Bird(), new Puppy() };
        for (int i = 0; i < 60; i++) {
            Animal a = zoo[i % zoo.Length];
            Mix((uint)(a.Sound(i) + a.Speak(i, i * 3L) + a.Plain(i)));
            Dog d = a as Dog; if (d != null) Mix((uint)d.Speak(i, 5));
            Puppy p = a as Puppy; if (p != null) Mix((uint)p.Plain(i));
        }
        Group("virtual dispatch");
        Animal nul = null; int caught = 0;
        for (int i = 0; i < 8; i++) {
            try { caught += nul.Sound(i); } catch (NullReferenceException) { caught += 1; }
            try { caught += nul.Plain(i) + 10; } catch (NullReferenceException) { caught += 100; }
            try { caught += nul.Speak(i, 1L) + 10; } catch (NullReferenceException) { caught += 1000; }
        }
        Mix((uint)caught);
        Group("null receivers");
        for (int n = 0; n < 36; n += 5) { Mix((uint)Rec(n, n, n + 1, n + 2, n * 0.25)); Mix((uint)(Even(n) * 10 + Odd(n))); }
        Mix((uint)Rec(30, 1, 2, 3, 4.0));
        Group("recursion");
        for (int d = 0; d < 30; d += 3) { fin = 0; Mix((uint)Catcher(d, d)); Mix((uint)fin); }
        Group("exceptions");
        // interface calls, casts, is and as, and loads from object arrays
        object[] objs = { new Ca(), new Cab(), new Cabd(), new Cre(), new Cexp(), new Cc(), new Cg(), new Cl(), "str", 5, null, new int[2], new string[1], new Dog() };
        int hits = 0;
        for (int i = 0; i < 65; i++) {
            object o = objs[i % objs.Length];
            IA ia = o as IA; IB ib = o as IB; IC ic = o as IC; Cab cab = o as Cab; Animal an = o as Animal;
            if (ia != null) { Mix((uint)ia.F(i)); hits += 1; }
            if (ib != null) { Mix((uint)ib.G(i)); hits += 10; }
            if (ic != null) { Mix((uint)(ic.H(i) + ic.F(i))); hits += 100; }
            if (cab != null) { Mix((uint)cab.F(i)); hits += 1000; }
            if (an != null) { Mix((uint)an.Sound(i)); hits += 10000; }
            Mix((o is IA ? 1u : 0u) + (o is IB ? 2u : 0u) + (o is IC ? 4u : 0u) + (o is Cab ? 8u : 0u) + (o is Cabd ? 16u : 0u) + (o is object[] ? 32u : 0u) + (o is Array ? 64u : 0u) + (o is string ? 128u : 0u));
            IG<int> gi = o as IG<int>; IG<long> gl = o as IG<long>;
            if (gi != null) Mix((uint)gi.Get(i)); if (gl != null) MixL(gl.Get(i * 1000003L));
        }
        Mix((uint)hits);
        int bad = 0;
        foreach (object o in objs) {
            try { IA xa = (IA)o; bad += xa == null ? 1000 : xa.F(1); } catch (InvalidCastException) { bad += 1; }
            try { Cab xb = (Cab)o; bad += xb == null ? 2000 : 2; } catch (InvalidCastException) { bad += 3; }
            try { object[] xc = (object[])o; bad += xc == null ? 4000 : xc.Length; } catch (InvalidCastException) { bad += 5; }
        }
        Mix((uint)bad);
        IA nia = null;
        for (int i = 0; i < 3; i++) { try { bad += nia.F(i); } catch (NullReferenceException) { bad += 1000; } }
        Mix((uint)bad);
        object[] oa = new object[8]; for (int i = 0; i < oa.Length; i++) { oa[i] = (i & 1) == 0 ? objs[i] : null; }
        for (int i = 0; i < 20; i++) { object o = oa[i & 7]; Mix(o == null ? 1u : (o == objs[i & 7] ? 2u : 3u)); }
        try { object o = oa[8]; Mix(o == null ? 1u : 2u); } catch (IndexOutOfRangeException) { Mix(99u); }
        Group("interfaces, casts, object arrays");
    }
}
