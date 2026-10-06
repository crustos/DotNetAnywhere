// Virtual calls in methods that are called by compiled code, and virtual targets that make virtual calls (native/src/WasmJIT.c): when a guard fails deep in a
// chain of compiled calls, every method on the chain gives the interpreter a frame, in the state that it is in, and the call that failed goes on in the
// innermost. Box<T> and ScoreG<T> are generic, so never among the types that the guards expect: they fail them, in each position, with every kind of state live.
// The output must equal Mono's. Floats are printed through (double).
using System;

struct V2 { public float X, Y; public V2(float x, float y) { X = x; Y = y; } }
abstract class Shape {
    public abstract float Area();
    public virtual int Sides() { return 0; }
    public virtual void Scale(float k) { }
    public virtual long Big() { return 1L << 40; }
    public virtual V2 Center() { return new V2(0f, 0f); }
    public virtual Shape Self() { return this; }
    public virtual float Weighted(float w, int n) { return Area() * w + n; }               // a virtual target that makes a virtual call
    public virtual float Twice(float w) { return Weighted(w, 1) + Weighted(w, 2); }         // ... and two levels of them
}
class Circle : Shape {
    public float R; public Circle(float r) { R = r; }
    public override float Area() { return 3f * R * R; }
    public override void Scale(float k) { R *= k; }
    public override V2 Center() { return new V2(R, -R); }
}
class Square : Shape {
    public float S; public Square(float s) { S = s; }
    public override float Area() { return S * S; }
    public override int Sides() { return 4; }
    public override void Scale(float k) { S *= k; }
    public override V2 Center() { return new V2(S, S); }
}
class Tri : Shape {
    public float B, H; public Tri(float b, float h) { B = b; H = h; }
    public override float Area() { return B * H * 0.5f; }
    public override int Sides() { return 3; }
    public override long Big() { return 7L; }
}
class Box<T> : Shape {
    public float V; public Box(float v) { V = v; }
    public override float Area() { return V + 1f; }
    public override int Sides() { return 9; }
    public override void Scale(float k) { V *= k; }
    public override V2 Center() { return new V2(V, V + 1f); }
    public override long Big() { return 99L; }
}
class Thrower : Shape {
    public bool Fail; public Thrower(bool f) { Fail = f; }
    public override float Area() { if (Fail) throw new InvalidOperationException("boom"); return 1f; }
}
interface IScore { int Score(); long Total(int k); }
class ScoreA : IScore { public int Score() { return 10; } public long Total(int k) { return k * 3L; } }
class ScoreB : IScore { public int Score() { return 20; } public long Total(int k) { return k * 5L + 1; } }
class ScoreG<T> : IScore { public int Score() { return 31; } public long Total(int k) { return -k; } }
struct Acc { public float V; public void Add(Shape s) { V += s.Area(); } }              // a struct method that makes a virtual call: not compiled into a caller

class WasmJitVirtualDeep {
    static double D(float f) { return f; }
    static double D(double d) { return d; }

    // ---- methods that are called, with a virtual call in them, returning and taking every kind of value
    static float AreaOf(Shape s) { return s.Area() * 2f; }
    static int SidesOf(Shape s, int k) { return s.Sides() + k; }
    static long BigPlus(Shape s, long k, double d) { double x = d * 2.0; long b = s.Big(); return b + k + (long)x; }
    static V2 Mid(Shape s, V2 off) { V2 c = s.Center(); c.X += off.X; c.Y -= off.Y; return c; }
    static Shape Pick(Shape a, Shape b, bool first) { Shape r = first ? a.Self() : b.Self(); return r; }
    static void Grow(Shape s, float k) { s.Scale(k); }
    static float Loop(Shape s, int n) { float t = 0f; for (int i = 0; i < n; i++) { t += s.Area() * i; if (i % 3 == 0) t -= s.Sides(); } return t; }
    static int ScoreOf(IScore s, int k) { return s.Score() + (int)s.Total(k); }
    static float Locals(Shape s, float seed) {                                              // state of every kind live across its own virtual calls
        long l = 5; double d = seed; V2 v = new V2(seed, 2f); byte b = 7; short sh = -3; int[] arr = new int[3]; float f = seed;
        for (int i = 0; i < 3; i++) { f += s.Area(); l += s.Big(); v.X += s.Center().X; arr[i] = s.Sides() + i; b += (byte)i; sh -= (short)arr[i]; d += 0.5; }
        return f + l + (float)d + v.X + v.Y + b + sh + arr[0] + arr[1] + arr[2];
    }

    // ---- two and three levels
    static float D2(Shape s, int n) { int k = n * 3; float r = AreaOf(s) + k; return r + Loop(s, 4); }
    static float D3(Shape[] a) { float t = 0f; for (int i = 0; i < a.Length; i++) t += D2(a[i], i) * 0.5f + AreaOf(a[i]); return t; }

    // ---- entry loops with state live, calling them
    static double Entry(Shape[] a, float seed) {
        long l = 3; double d = seed; V2 v = new V2(1f, 2f); float acc = seed; int n = 0; int[] arr = new int[a.Length];
        for (int i = 0; i < a.Length; i++) {
            acc += AreaOf(a[i]);
            l += BigPlus(a[i], i, 2.5);
            V2 m = Mid(a[i], v); v.X += m.X; v.Y -= m.Y * 0.25f;
            d += Loop(a[i], 3);
            Grow(a[i], 1.01f);
            n += SidesOf(a[i], i);
            arr[i] = SidesOf(a[i], 1) * 2;
            acc = 1f + acc * AreaOf(a[i]) * 0.001f;
        }
        double after = 0; for (int i = 0; i < arr.Length; i++) after += arr[i] * (i + 1);
        return acc + l + d + v.X + v.Y + n + after;
    }
    static float StackBelow(Shape[] a) { return 1f + AreaOf(a[0]) * (2f + AreaOf(a[1])) - AreaOf(a[2]); }
    static float Locs(Shape[] a) { float t = 0f; for (int i = 0; i < a.Length; i++) t += Locals(a[i], i); return t; }
    static int Scores(IScore[] a) { int t = 0; for (int i = 0; i < a.Length; i++) t += ScoreOf(a[i], i); return t; }
    static float WeighAll(Shape[] a) { float t = 0f; for (int i = 0; i < a.Length; i++) t += a[i].Weighted(0.5f, i); return t; }
    static float TwiceAll(Shape[] a) { float t = 0f; for (int i = 0; i < a.Length; i++) t += a[i].Twice(0.25f) + a[i].Sides(); return t; }
    static float TwoLoops(Shape[] a, Shape[] b) {
        float t = 0f; int k = 0;
        do { t += AreaOf(a[k]); k++; } while (k < a.Length);
        t *= 2f;
        int j = 0;
        do { t += SidesOf(b[j], j) * 3f + AreaOf(b[j]); j++; } while (j < b.Length);
        return t;
    }
    static float WithStruct(Shape[] a) { Acc acc = default(Acc); for (int i = 0; i < a.Length; i++) acc.Add(a[i]); return acc.V; }

    static string Try(Func<float> f) { try { return D(f()).ToString(); } catch (NullReferenceException) { return "NRE"; } catch (InvalidOperationException e) { return e.Message; } }

    static Shape[] Mixed(int n, int oddAt, bool generic) {
        Shape[] a = new Shape[n];
        for (int i = 0; i < n; i++) a[i] = i == oddAt ? (generic ? (Shape)new Box<int>(i + 2f) : new Circle(9f)) : i % 3 == 0 ? (Shape)new Circle(i + 1f) : i % 3 == 1 ? (Shape)new Square(i * 0.5f + 1f) : new Tri(i, 2f);
        return a;
    }

    static void Main() {
        Shape[] plain = Mixed(7, -1, false);
        Console.WriteLine("plain " + D(D3(plain)) + " " + D(Entry(plain, 1f)) + " " + D(StackBelow(plain)) + " " + D(Locs(plain)) + " " + D(WeighAll(plain)) + " " + D(TwiceAll(plain)));
        IScore[] sc = new IScore[6]; for (int i = 0; i < 6; i++) sc[i] = i % 2 == 0 ? (IScore)new ScoreA() : new ScoreB();
        Console.WriteLine("iface " + Scores(sc));
        Console.WriteLine("two " + D(TwoLoops(Mixed(3, -1, false), Mixed(4, -1, false))) + " struct " + D(WithStruct(plain)));
        // a receiver that no guard expects, at each position, in each of the kernels
        for (int pos = 0; pos < 7; pos++) {
            Shape[] odd = Mixed(7, pos, true);
            Console.WriteLine("odd@" + pos + " " + D(D3(odd)) + " " + D(Entry(Mixed(7, pos, true), 2f)) + " " + D(Locs(odd)) + " " + D(WeighAll(odd)) + " " + D(TwiceAll(odd)) + " " + D(WithStruct(odd)));
        }
        for (int pos = 0; pos < 3; pos++) {
            Shape[] odd = Mixed(3, pos, true);
            Console.WriteLine("below@" + pos + " " + D(StackBelow(odd)));
        }
        IScore[] sg = new IScore[6]; for (int i = 0; i < 6; i++) sg[i] = i == 3 ? (IScore)new ScoreG<int>() : sc[i];
        Console.WriteLine("iface-odd " + Scores(sg) + " " + Scores(sc));
        for (int pos = 0; pos < 4; pos++) {
            Shape[] first = Mixed(3, -1, false), second = Mixed(4, pos, true);
            Console.WriteLine("two@" + pos + " " + D(TwoLoops(first, second)) + " " + D(TwoLoops(first, second)));
        }
        // single arguments of each kind through a failing call
        Shape bx = new Box<string>(5f), ci = new Circle(2f);
        Console.WriteLine("args " + D(AreaOf(bx)) + " " + SidesOf(bx, 3) + " " + BigPlus(bx, 10, 1.5) + " " + D(Mid(bx, new V2(1f, 2f)).X) + " " + D(Mid(bx, new V2(1f, 2f)).Y) + " " + D(Pick(ci, bx, false).Area()) + " " + D(Pick(ci, bx, true).Area()) + " " + D(Loop(bx, 7)) + " " + D(Locals(bx, 1.5f)));
        Grow(bx, 3f); Console.WriteLine("void " + D(((Box<string>)bx).V));
        // exceptions: from the failed call itself, after the frames were handed over, and through all of them
        Shape[] thr = { new Circle(1f), new Thrower(false), new Thrower(true), new Square(3f) };
        Shape[] thrOdd = { new Circle(1f), new Box<int>(1f), new Thrower(true), new Square(3f) };
        Shape[] withNull = { new Circle(1f), null, new Square(3f) };
        Console.WriteLine("errors " + Try(() => D3(thr)) + " " + Try(() => D3(thrOdd)) + " " + Try(() => D3(withNull)) + " " + Try(() => (float)Entry(thrOdd, 1f)) + " " + Try(() => WeighAll(thrOdd)) + " " + Try(() => TwiceAll(withNull)));
        // after all of that, the methods must still be right
        Console.WriteLine("again " + D(D3(plain)) + " " + D(Entry(Mixed(7, -1, false), 1f)) + " " + Scores(sc) + " " + D(WeighAll(plain)));
    }
}
