using System;
using System.Collections.Generic;

// Methods with a cold path inlined into their callers' blocks (see StencilInline.cs and StencilIslands.cs): the hot path is stencils in the
// caller's block and the cold one (a throw, a call that is rarely needed, a lazy initialisation) is an island that the interpreter runs in
// the caller's frame, after which the block goes on. This checks: List<T> from another assembly (its throw has its own ldstr: the string
// has to be found in the metadata of the method it came from), a cold call that comes back (lazy initialisation, growing a list), an
// exception thrown from an inlined cold path crossing frames and being caught, callers that have handlers (which must not use islands),
// nesting, and final / sealed methods called through callvirt. Compared with Mono line for line; also run with DNA_NO_INLINE=1 and
// DNA_NO_ISLANDS=1 by the suite.
interface IShape { int Area(int k); int Name { get; } }
sealed class Sq : IShape { public int side; public int Area(int k) { return side * side + k; } public int Name { get { return 7; } } }
class Rc : IShape { public int w, h; public virtual int Area(int k) { return w * h + k; } public int Name { get { return w; } } }
class Rc2 : Rc { public override int Area(int k) { return w * h * 2 + k; } }
class Program {
    static uint h;
    static void Mix(uint v) { h = (h ^ v) * 16777619u; }
    static void MixL(long v) { Mix((uint)v); Mix((uint)(v >> 32)); }
    static void Group(string name) { Console.WriteLine(name + " " + h); h = 2166136261u; }

    static int[] cache; static int builds;
    static int[] Build() { builds++; int[] a = new int[8]; for (int i = 0; i < a.Length; i++) a[i] = i * i + 1; return a; }
    static int Lazy(int k) { if (cache == null) { cache = Build(); } return cache[k & 7]; }                        // a cold call that comes back
    static int Checked(int[] a, int i) { if (i < 0 || i >= a.Length) { throw new ArgumentOutOfRangeException("idx" + i); } return a[i] * 2; }   // a cold throw
    static int Validate(int x, int lo, int hi) { if (x < lo) throw new InvalidOperationException("low " + x); if (x > hi) throw new InvalidOperationException("high " + x); return x - lo; }
    static int Outer(int[] a, int i) { return Checked(a, i) + Validate(i, -2, 5); }                                // nested: both have cold paths
    static string Describe(int k) { return k == 0 ? "zero" : (k < 0 ? "negative" : "positive"); }               // an ldstr on every path
    static string BoolText(bool b) { return b.ToString(); }                                                       // corlib's, with its own strings
    static object MakeBox(int k) { if (k > 100) { throw new InvalidOperationException("big"); } return k; }

    // callers without handlers (so the cold paths of what they call are inlined into them as islands)
    static int Last(int a, int b) { return Math.Max(a, b); }                                                       // the inlined code is the last thing in the block
    static int noted;
    static void Note() { noted++; }
    // an island earlier in the block and an inlined recipe that jumps to its end as the last thing in it: that jump must land after the recipe,
    // not on the block's first exit (which would run the Note call)
    static int Last2(int a, int b) { if (a == 12345) { Note(); } return Math.Max(a, b); }
    static int Last3(int a, int b, int c) { int r = a + b; if (r < -50) { Note(); } r += c; if (r == 999) { Note(); } return Math.Max(r, c) + Math.Max(a, 2); }
    static int Words(int n) { int s = 0; for (int i = -n; i < n; i++) { s += Describe(i).Length + BoolText((i & 1) == 0).Length + Last(i, 3) + Math.Max(i, -2); } return s; }
    static long Lists(List<int> l, int n) { long s = 0; for (int i = 0; i < n; i++) { l.Add(i * 7); s += l[i / 2] + l.Count; } return s; }
    static int Short(List<short> l, List<byte> b, List<char> c, int n) {
        int s = 0;
        for (int i = 0; i < n; i++) { l.Add((short)(i * 300 - 9000)); b.Add((byte)(i * 3)); c.Add((char)('a' + (i & 15))); }
        for (int i = 0; i < n; i++) { s += l[i] + b[i] * 7 + c[i]; }
        return s;
    }
    static int OutOfRange(List<int> l, int i) { return l[i] + 1; }
    static int Catch2(List<int> l, int n) { int bad = 0; for (int i = -2; i < n; i++) { try { bad += OutOfRange(l, i); } catch (ArgumentOutOfRangeException e) { bad += 100 + e.ParamName.Length * 7 + e.ParamName[0]; } } return bad; }

    static int WithHandler(int[] a, int n) {                                                                       // has a handler: its calls are not islands
        int s = 0;
        for (int i = 0; i < n; i++) { try { s += Checked(a, i - 2); s ^= i << 1; s += Validate(i, 0, 3) * 3; } catch (ArgumentOutOfRangeException e) { s += e.ParamName.Length; } catch (InvalidOperationException e) { s += e.Message.Length * 7; } }
        return s;
    }
    static int Thrower(int[] a, int n) { int s = 0; for (int i = 0; i < n; i++) { s += Outer(a, i - 3) + Lazy(i); } return s; }   // lets the exception through
    static int Catcher(int[] a, int n) { int s = 0, bad = 0; for (int i = 0; i < n; i++) { try { s += Thrower(a, i); } catch (ArgumentOutOfRangeException e) { bad += 1 + e.ParamName.Length; } catch (InvalidOperationException e) { bad += 100 + e.Message.Length; } } return s * 1000 + bad; }

    public static void Main() {
        h = 2166136261u;
        int[] data = { 3, 1, 4, 1, 5, 9, 2, 6 };
        for (int i = -3; i < 12; i++) {
            try { Mix((uint)Checked(data, i)); } catch (ArgumentOutOfRangeException e) { Mix((uint)e.ParamName.Length); Mix((uint)e.ParamName[e.ParamName.Length - 1]); }
            try { Mix((uint)Validate(i, 0, 6)); } catch (InvalidOperationException e) { Mix((uint)e.Message.Length); Mix((uint)e.Message[0]); }
            Mix((uint)Lazy(i)); Mix((uint)Describe(i).Length); Mix((uint)BoolText((i & 1) == 0).Length);
            try { Mix((uint)(int)MakeBox(i * 20)); } catch (InvalidOperationException e) { Mix((uint)e.Message.Length); }
        }
        Mix((uint)builds);
        Group("cold throws and lazy initialisation");
        for (int n = 0; n < 14; n++) { Mix((uint)Catcher(data, n)); Mix((uint)WithHandler(data, n)); }
        Mix((uint)builds);
        Group("through frames and handlers");
        // List<T> (corlib): growth is a cold call that comes back, an index out of range is a cold throw with a string from corlib's metadata
        List<int> list = new List<int>();
        long sum = 0; int caught = 0;
        for (int i = 0; i < 300; i++) { list.Add(i * 3 + 1); }
        for (int i = 0; i < list.Count; i++) { sum += list[i]; }
        for (int i = -2; i < 6; i++) { try { sum += list[list.Count + i]; } catch (ArgumentOutOfRangeException e) { caught += 1 + e.ParamName.Length; } }
        for (int i = 0; i < 5; i++) { try { sum += list[-1 - i]; } catch (ArgumentOutOfRangeException e) { caught += 10 + e.ParamName.Length; } }
        MixL(sum); Mix((uint)caught); Mix((uint)list.Count);
        List<string> names = new List<string>(); for (int i = 0; i < 50; i++) { names.Add("n" + i); }
        int total = 0; for (int i = 0; i < names.Count; i++) { total += names[i].Length; } Mix((uint)total);
        List<float> fl = new List<float>(); for (int i = 0; i < 40; i++) { fl.Add(i * 0.5f); } float fs = 0; for (int i = 0; i < fl.Count; i++) { fs += fl[i]; } Mix((uint)(int)(fs * 10f));
        List<long> ll = new List<long>(); for (int i = 0; i < 40; i++) { ll.Add((long)i << 33); } long ls = 0; for (int i = 0; i < ll.Count; i++) { ls += ll[i]; } MixL(ls);
        Group("List<T>");
        // final and sealed methods through callvirt, interface calls, virtual calls that must stay virtual
        IShape[] shapes = { new Sq { side = 3 }, new Rc { w = 2, h = 5 }, new Rc2 { w = 2, h = 5 }, new Sq { side = 4 } };
        Sq sq = new Sq { side = 5 }; Rc rc = new Rc2 { w = 3, h = 4 };
        long acc = 0;
        for (int i = 0; i < 60; i++) { IShape s = shapes[i & 3]; acc += s.Area(i) + s.Name; acc += sq.Area(i) + rc.Area(i) + sq.Name; }
        Sq nul = null; try { acc += nul.Area(1); } catch (NullReferenceException) { acc += 1000; }
        MixL(acc);
        List<int> lst = new List<int>(); MixL(Lists(lst, 100)); MixL(Lists(lst, 40)); Mix((uint)lst.Count); Mix((uint)Catch2(lst, 10)); Mix((uint)Catch2(lst, 300));
        for (int n = 0; n < 8; n++) { Mix((uint)Words(n)); }
        for (int i = -6; i < 8; i++) { Mix((uint)Last2(i * 3, 4)); Mix((uint)Last3(i * 5, -i, 7)); } Mix((uint)Last2(12345, 1)); Mix((uint)Last3(500, 494, 5)); Mix((uint)noted);
        Mix((uint)Short(new List<short>(), new List<byte>(), new List<char>(), 70));
        Group("callers without handlers");
        Group("final, sealed and virtual");
    }
}
