// Virtual and interface calls in the wasm JIT (native/src/WasmJIT.c): each is compiled for the targets that the types that exist can reach, with a
// guard on the receiver. A receiver that fails every guard hands the frame to the interpreter, which goes on from the call. A generic class is not among
// the types that are enumerated, so Box<T> and ScoreG<T> always fail the guards: they force that path, at every point and with every kind of state live.
// The output must equal Mono's. Floats are printed through (double).
using System;

struct Vec2 { public float X, Y; public Vec2(float x, float y) { X = x; Y = y; } }
struct Wide { public float A, B, C, D, E, F, G, H, I, J, K, L, M, N; }          // too many leaves to be held in locals: stays in memory

abstract class Shape {
    public abstract float Area();
    public virtual int Sides() { return 0; }
    public virtual void Scale(float k) { }
    public virtual long Big() { return 1L << 40; }
    public virtual double Ratio() { return 0.5; }
    public virtual Vec2 Center() { return new Vec2(0f, 0f); }
    public virtual Shape Self() { return this; }
    public virtual float Weighted(float w, int n) { return Area() * w + n; }
    public virtual float Dot(Vec2 v) { return v.X + v.Y; }
}
class Circle : Shape {
    public float R; public Circle(float r) { R = r; }
    public override float Area() { return 3f * R * R; }
    public override void Scale(float k) { R *= k; }
    public override Vec2 Center() { return new Vec2(R, -R); }
    public override float Dot(Vec2 v) { return R * v.X - v.Y; }
}
class Square : Shape {
    public float S; public Square(float s) { S = s; }
    public override float Area() { return S * S; }
    public override int Sides() { return 4; }
    public override void Scale(float k) { S *= k; }
    public override Vec2 Center() { return new Vec2(S, S); }
    public override double Ratio() { return 1.5; }
}
class Tri : Shape {
    public float B, H; public Tri(float b, float h) { B = b; H = h; }
    public override float Area() { return B * H * 0.5f; }
    public override int Sides() { return 3; }
    public override long Big() { return 7L; }
}
class Poly : Shape {                                                    // a target with a loop: it is called, not inlined
    public int N; public Poly(int n) { N = n; }
    public override float Area() { float t = 0f; for (int i = 0; i < N; i++) t += i * 0.5f; return t; }
    public override int Sides() { return N; }
}
class Box<T> : Shape {                                                  // generic: never predicted
    public float V; public Box(float v) { V = v; }
    public override float Area() { return V + 1f; }
    public override int Sides() { return 9; }
    public override void Scale(float k) { V *= k; }
    public override Vec2 Center() { return new Vec2(V, V + 1f); }
    public override long Big() { return 99L; }
    public override double Ratio() { return 2.5; }
    public override float Dot(Vec2 v) { return V * 2f; }
}
class Thrower : Shape {
    public bool Fail; public Thrower(bool f) { Fail = f; }
    public override float Area() { if (Fail) throw new InvalidOperationException("boom"); return 1f; }
}
interface IScore { int Score(); long Total(int k); }
class ScoreA : IScore { public int Score() { return 10; } public long Total(int k) { return k * 3L; } }
class ScoreB : IScore { public int Score() { return 20; } public long Total(int k) { return k * 5L + 1; } }
class ScoreG<T> : IScore { public int Score() { return 31; } public long Total(int k) { return -k; } }
abstract class Animal { public abstract int Sound(); }
class A1 : Animal { public override int Sound() { return 1; } }
class A2 : Animal { public override int Sound() { return 2; } }
class A3 : Animal { public override int Sound() { return 3; } }
class A4 : Animal { public override int Sound() { return 4; } }
class A5 : Animal { public override int Sound() { return 5; } }
class A6 : Animal { public override int Sound() { return 6; } }
class A7 : Animal { public override int Sound() { return 7; } }
class A8 : Animal { public override int Sound() { return 8; } }

class Holder { public float V; public virtual float Twice() { return V * 2f; } public float Run() { return Twice() + Twice() * 0.5f; } }
class Holder2 : Holder { public override float Twice() { return V * 3f; } }
class HolderG<T> : Holder { public override float Twice() { return V * 5f; } }

class WasmJitVirtual {
    static double D(float f) { return f; }
    static double D(double d) { return d; }

    static float SumAreas(Shape[] a) { float t = 0f; for (int i = 0; i < a.Length; i++) t += a[i].Area(); return t; }
    static int SumSides(Shape[] a) { int t = 0; for (int i = 0; i < a.Length; i++) t += a[i].Sides(); return t; }

    // every kind of state live across a call that may deoptimize, and code after the loop that the interpreter then runs
    static double Mixed(Shape[] a, float seed) {
        long l = 5; double d = seed; Vec2 v = new Vec2(1f, 2f); Wide w = default(Wide); float acc = seed; int[] arr = new int[a.Length + 1]; byte b = 7; short sh = -3; Vec2 keep = new Vec2(seed, -seed);
        for (int i = 0; i < a.Length; i++) {
            acc += a[i].Area();
            l += a[i].Big();
            d += a[i].Ratio();
            v.X += a[i].Center().X;
            w.A += i; w.N += a[i].Sides(); w.G = acc;
            acc = acc * 0.5f + (i % 3) * a[i].Area() * 0.25f;
            arr[i] = a[i].Sides() + i; b += (byte)i; sh -= (short)a[i].Sides(); keep.Y += 1f;
        }
        double after = 0; for (int i = 0; i < arr.Length; i++) after += arr[i] * (i + 1);
        return acc + l + d + v.X + v.Y + w.A + w.G + w.N + after + b + sh + keep.X + keep.Y;
    }
    static float StackBelow(Shape[] a) { return 1f + a[0].Area() * (2f + a[1].Area()) - a[2].Area(); }
    static float StructArg(Shape[] a) { float t = 0f; Vec2 v = new Vec2(2f, 3f); for (int i = 0; i < a.Length; i++) { t += a[i].Dot(v); v.X += 1f; } return t; }
    static float Chain(Shape s) { return s.Self().Self().Area() + s.Self().Sides(); }
    static float Nested(Shape[] a, int n) { float t = 0f; for (int i = 0; i < n; i++) for (int j = 0; j < a.Length; j++) { t += a[j].Area() * (i + 1); if (j == 1) t -= a[j].Sides(); } return t; }
    static float Scaling(Shape[] a) { float t = 0f; for (int i = 0; i < a.Length; i++) { a[i].Scale(2f); t += a[i].Area(); } return t; }
    // two loops in a row, the first a do-while (its body runs once more if it is entered again): a miss in the second must go back into the second
    static float TwoLoops(Shape[] a, Shape[] b) {
        float t = 0f; int k = 0;
        do { t += a[k].Area(); k++; } while (k < a.Length);
        t *= 2f;
        int j = 0; float u = 0f;
        do { u += b[j].Sides() * 3f + b[j].Area(); j++; t += 0.5f; } while (j < b.Length);
        for (int x = 0; x < b.Length; x++) u += b[x].Sides();
        return t + u;
    }
    static float OnThis(Holder h) { return h.Run(); }
    static float Weigh(Shape[] a) { float t = 0f; for (int i = 0; i < a.Length; i++) t += a[i].Weighted(0.5f, i); return t; }
    static float Branches(Shape[] a, int mode) {
        float t = 0f;
        for (int i = 0; i < a.Length; i++) { if ((i + mode) % 3 == 0) t += a[i].Area(); else if ((i + mode) % 3 == 1) t -= a[i].Sides(); else t *= 1.01f; }
        return t;
    }

    static int SumScores(IScore[] a) { int t = 0; for (int i = 0; i < a.Length; i++) t += a[i].Score(); return t; }
    static long TotalK(IScore[] a, int k) { long t = 0; for (int i = 0; i < a.Length; i++) t += a[i].Total(k + i) * 2; return t; }
    static int Chorus(Animal[] a) { int t = 0; for (int i = 0; i < a.Length; i++) t += a[i].Sound(); return t; }       // too many targets: not compiled

    static string Try(Func<float> f) { try { return D(f()).ToString(); } catch (NullReferenceException) { return "NRE"; } catch (InvalidOperationException e) { return e.Message; } catch (IndexOutOfRangeException) { return "IOOR"; } }

    static void Main() {
        // all of one type: every guard hits
        Shape[] circles = new Shape[8]; for (int i = 0; i < 8; i++) circles[i] = new Circle(i + 1f);
        Console.WriteLine("mono " + D(SumAreas(circles)) + " " + SumSides(circles) + " " + D(Mixed(circles, 1f)) + " " + D(StructArg(circles)) + " " + D(Nested(circles, 3)) + " " + D(Branches(circles, 0)));
        // several types, all counted
        Shape[] mixed = new Shape[10];
        for (int i = 0; i < 10; i++) mixed[i] = i % 4 == 0 ? (Shape)new Circle(i + 1f) : i % 4 == 1 ? (Shape)new Square(i * 0.5f) : i % 4 == 2 ? (Shape)new Tri(i, 2f) : (Shape)new Poly(i + 3);
        Console.WriteLine("poly " + D(SumAreas(mixed)) + " " + SumSides(mixed) + " " + D(Mixed(mixed, 2f)) + " " + D(StructArg(mixed)) + " " + D(Nested(mixed, 3)) + " " + D(Branches(mixed, 1)));
        Console.WriteLine("calls " + D(StackBelow(mixed)) + " " + D(Chain(mixed[2])) + " " + D(Scaling(mixed)) + " " + D(SumAreas(mixed)));
        Holder h = new Holder(); h.V = 3f; Holder2 h2 = new Holder2(); h2.V = 4f; Console.WriteLine("this " + D(OnThis(h)) + " " + D(OnThis(h2)) + " " + D(Weigh(mixed)));
        // an object whose type is generic: fails every guard, anywhere in the loop, with all that state live
        for (int pos = 0; pos < 10; pos += 3) {
            Shape[] odd = new Shape[10];
            for (int i = 0; i < 10; i++) odd[i] = i == pos ? (Shape)new Box<int>(i + 2f) : mixed[i];
            Console.WriteLine("deopt@" + pos + " " + D(SumAreas(odd)) + " " + SumSides(odd) + " " + D(Mixed(odd, 3f)) + " " + D(StructArg(odd)) + " " + D(Nested(odd, 2)) + " " + D(Branches(odd, 2)));
        }
        Shape[] three = { new Circle(1f), new Box<string>(2f), new Square(3f) };
        Console.WriteLine("below " + D(StackBelow(three)) + " " + D(StackBelow(new Shape[] { new Box<int>(1f), new Square(2f), new Circle(3f) })) + " " + D(StackBelow(new Shape[] { new Circle(1f), new Square(2f), new Box<int>(3f) })));
        Console.WriteLine("chain " + D(Chain(new Box<int>(4f))) + " " + D(Scaling(new Shape[] { new Box<int>(1f), new Circle(2f) })));
        HolderG<int> hb = new HolderG<int>(); hb.V = 5f; Console.WriteLine("this-deopt " + D(OnThis(hb)) + " " + D(Weigh(new Shape[] { new Box<int>(2f), new Circle(1f) })));
        // a null receiver, and a target that throws
        Shape[] withNull = { new Circle(1f), null, new Square(2f) };
        Console.WriteLine("errors " + Try(() => SumAreas(withNull)) + " " + Try(() => SumAreas(null)) + " " + Try(() => SumAreas(new Shape[] { new Thrower(false), new Thrower(false) })) + " " + Try(() => SumAreas(new Shape[] { new Thrower(false), new Thrower(true), new Circle(1f) })) + " " + Try(() => SumAreas(new Shape[] { new Box<int>(1f), new Thrower(true) })));
        // interfaces: counted types, then a generic one
        IScore[] sc = new IScore[9]; for (int i = 0; i < 9; i++) sc[i] = i % 2 == 0 ? (IScore)new ScoreA() : new ScoreB();
        Console.WriteLine("iface " + SumScores(sc) + " " + TotalK(sc, 5));
        IScore[] sg = new IScore[9]; for (int i = 0; i < 9; i++) sg[i] = i == 4 ? (IScore)new ScoreG<int>() : sc[i];
        Console.WriteLine("iface-deopt " + SumScores(sg) + " " + TotalK(sg, 5) + " " + SumScores(new IScore[] { new ScoreG<long>() }));
        // too many targets for the guards: the method is simply interpreted
        Animal[] zoo = { new A1(), new A2(), new A3(), new A4(), new A5(), new A6(), new A7(), new A8() };
        Console.WriteLine("zoo " + Chorus(zoo) + " " + Chorus(new Animal[0]));
        // many different unexpected types, one after another: the method is compiled again with each until there are too many targets, or it has been
        // compiled again enough; after that a type that is not among the targets deoptimizes every time. The results are the same all through.
        Shape[] many = new Shape[12];
        many[0] = new Box<int>(1f); many[1] = new Box<long>(2f); many[2] = new Box<short>(3f); many[3] = new Box<byte>(4f); many[4] = new Box<char>(5f); many[5] = new Box<double>(6f);
        many[6] = new Box<float>(7f); many[7] = new Box<string>(8f); many[8] = new Box<bool>(9f); many[9] = new Circle(10f); many[10] = new Square(11f); many[11] = new Box<object>(12f);
        for (int round = 0; round < 3; round++) {
            for (int upto = 1; upto <= 12; upto += 2) {
                Shape[] part = new Shape[upto]; Array.Copy(many, part, upto);
                Console.WriteLine("many " + round + "/" + upto + " " + D(SumAreas(part)) + " " + SumSides(part) + " " + D(Mixed(part, 1f)) + " " + D(Nested(part, 2)));
            }
        }
        // a miss in the inner loop of a nest, at each position: the frame goes on in the interpreter and comes back into the compiled code at the outer loop
        for (int pos = 0; pos < 5; pos++) {
            Shape[] row = new Shape[5];
            for (int i = 0; i < 5; i++) row[i] = i == pos ? (Shape)new Box<decimal>(i + 0.5f) : new Square(i + 1f);
            Console.WriteLine("inner " + pos + " " + D(Nested(row, 6)) + " " + D(Nested(row, 6)) + " " + D(Scaling(row)));
        }
        for (int pos = 0; pos < 6; pos++) {
            Shape[] first = { new Circle(1f), new Square(2f), new Tri(3f, 4f) };
            Shape[] second = new Shape[6];
            for (int i = 0; i < 6; i++) second[i] = i == pos ? (Shape)new Box<ushort>(i + 1f) : (i % 2 == 0 ? (Shape)new Square(i + 1f) : (Shape)new Poly(i + 2));
            Console.WriteLine("two " + pos + " " + D(TwoLoops(first, second)) + " " + D(TwoLoops(first, second)));
        }
        // and again, after all that: compiled code must be as good as it was
        Console.WriteLine("again " + D(SumAreas(circles)) + " " + D(Mixed(mixed, 2f)) + " " + SumScores(sc));
    }
}
