// Virtual and interface calls in loops: what speculative devirtualization (and inlining of the guarded targets) is for.
using System;
using System.Diagnostics;

struct V2 { public float X, Y; public V2(float x, float y) { X = x; Y = y; } }
abstract class Shape { public abstract float Area(); public abstract V2 Center(); public virtual int Sides() { return 0; } }
class Circle : Shape { public float R; public Circle(float r) { R = r; } public override float Area() { return 3.14159f * R * R; } public override V2 Center() { return new V2(R, -R); } }
class Square : Shape { public float S; public Square(float s) { S = s; } public override float Area() { return S * S; } public override V2 Center() { return new V2(S, S); } public override int Sides() { return 4; } }
class Tri : Shape { public float B, H; public Tri(float b, float h) { B = b; H = h; } public override float Area() { return B * H * 0.5f; } public override V2 Center() { return new V2(B / 3f, H / 3f); } public override int Sides() { return 3; } }
class Box<T> : Shape { public float V; public Box(float v) { V = v; } public override float Area() { return V; } public override V2 Center() { return new V2(V, V); } }   // generic: never predicted
interface IGet { int Get(int k); }
class GetA : IGet { public int Get(int k) { return k + 1; } }
class GetB : IGet { public int Get(int k) { return k * 2; } }

class VirtualKernels {
    static float SumAreas(Shape[] a, int reps) { float t = 0f; for (int r = 0; r < reps; r++) for (int i = 0; i < a.Length; i++) t += a[i].Area(); return t; }
    static float Centers(Shape[] a, int reps) { float t = 0f; for (int r = 0; r < reps; r++) for (int i = 0; i < a.Length; i++) { V2 c = a[i].Center(); t += c.X - c.Y; } return t; }
    static int Sides(Shape[] a, int reps) { int t = 0; for (int r = 0; r < reps; r++) for (int i = 0; i < a.Length; i++) t += a[i].Sides(); return t; }
    static long Iface(IGet[] a, int reps) { long t = 0; for (int r = 0; r < reps; r++) for (int i = 0; i < a.Length; i++) t += a[i].Get(i); return t; }
    static float AreaOf(Shape s) { return s.Area() * 2f; }                                   // a virtual call in a method that is called
    static float Weighted(Shape s, float w) { return s.Area() * w + s.Sides(); }             // two of them
    static float ViaCallee(Shape[] a, int reps) { float t = 0f; for (int r = 0; r < reps; r++) for (int i = 0; i < a.Length; i++) t += AreaOf(a[i]); return t; }
    static float ViaTwo(Shape[] a, int reps) { float t = 0f; for (int r = 0; r < reps; r++) for (int i = 0; i < a.Length; i++) t += Weighted(a[i], 0.5f); return t; }
    static void Report(string name, Stopwatch sw, string v) { Console.WriteLine(name + ": " + sw.ElapsedMilliseconds + " ms  checksum=" + v); }

    static void Main() {
        Shape[] mono = new Shape[1000], poly = new Shape[1000]; IGet[] ifc = new IGet[1000]; Shape[] withBox = new Shape[1000];
        for (int i = 0; i < 1000; i++) {
            mono[i] = new Circle(i * 0.01f + 1f);
            poly[i] = i % 3 == 0 ? (Shape)new Circle(i * 0.01f + 1f) : i % 3 == 1 ? (Shape)new Square(i * 0.02f + 1f) : (Shape)new Tri(i * 0.01f + 1f, 2f);
            ifc[i] = i % 2 == 0 ? (IGet)new GetA() : new GetB();
            withBox[i] = i == 500 ? (Shape)new Box<int>(7f) : poly[i];
        }
        for (int round = 0; round < 3; round++) {
            Stopwatch sw = Stopwatch.StartNew(); float f = SumAreas(mono, 6000); Report("monomorphic area   ", sw, ((double)f).ToString());
            sw = Stopwatch.StartNew(); f = SumAreas(poly, 6000); Report("3 types area        ", sw, ((double)f).ToString());
            sw = Stopwatch.StartNew(); f = Centers(poly, 6000); Report("struct-returning     ", sw, ((double)f).ToString());
            sw = Stopwatch.StartNew(); int n = Sides(poly, 6000); Report("virtual int         ", sw, n.ToString());
            sw = Stopwatch.StartNew(); long l = Iface(ifc, 6000); Report("interface           ", sw, l.ToString());
            sw = Stopwatch.StartNew(); f = ViaCallee(poly, 6000); Report("via a callee        ", sw, ((double)f).ToString());
            sw = Stopwatch.StartNew(); f = ViaTwo(poly, 6000); Report("callee, 2 virtuals  ", sw, ((double)f).ToString());
            sw = Stopwatch.StartNew(); f = ViaCallee(withBox, 6000); Report("callee + deopt      ", sw, ((double)f).ToString());
            sw = Stopwatch.StartNew(); f = SumAreas(withBox, 6000); Report("deopt (1 of 1000)   ", sw, ((double)f).ToString());
        }
    }
}
