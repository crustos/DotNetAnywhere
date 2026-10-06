// Allocation from methods that the wasm JIT compiles (new of classes, newarr, stelem.ref), under garbage-collection pressure: the
// references live in wasm locals while the code runs, which the collector cannot see, so it is held off while it runs and runs afterwards. The
// survivors are checked after collections that happen in between. Output must equal Mono's.
using System;

class Node { public int Value; public Node Next; public Node(int v, Node n) { Value = v; Next = n; } }
class Pair {
    public float A, B; public int[] Data;
    public Pair() { A = 1f; B = 2f; }
    public Pair(float a, float b, int n) { A = a; B = b; Data = new int[n]; for (int i = 0; i < n; i++) Data[i] = i * (int)a; }
    public float Sum() { float s = A + B; if (Data != null) for (int i = 0; i < Data.Length; i++) s += Data[i]; return s; }
}
class Bad { public int V; public Bad(Node n) { V = n.Value; } }        // throws when n is null, after the object exists
struct Cell { public int X; public float Y; public Node Link; }

class WasmJitAlloc {
    static Node BuildList(int n) { Node head = null; for (int i = 0; i < n; i++) head = new Node(i, head); return head; }
    static long SumList(Node h) { long s = 0; while (h != null) { s += h.Value; h = h.Next; } return s; }
    static int Length(Node h) { int n = 0; while (h != null) { n++; h = h.Next; } return n; }

    // many lists built and dropped (garbage), every 5th kept in an array that is itself allocated here
    static Node[] Churn(int rounds, int n) {
        Node[] keep = new Node[rounds];
        for (int r = 0; r < rounds; r++) {
            Node h = BuildList(n + r);
            if (r % 5 == 0) keep[r] = h;
        }
        return keep;
    }
    static long SumKept(Node[] keep) { long s = 0; for (int i = 0; i < keep.Length; i++) s += SumList(keep[i]) * (i + 1) + Length(keep[i]); return s; }

    // a reference held in a local across a lot of allocation, in the entry function (which yields)
    static long Hold(int rounds) {
        Node anchor = new Node(12345, new Node(678, null));
        long acc = 0;
        for (int r = 0; r < rounds; r++) {
            Pair p = new Pair(r, 0.5f, 20);
            Node tmp = BuildList(50);
            acc += (long)(p.Sum() * 10f) + SumList(tmp);
        }
        return acc + anchor.Value + anchor.Next.Value;
    }

    static float[][] Jagged(int rows, int cols) {
        float[][] g = new float[rows][];
        for (int r = 0; r < rows; r++) { g[r] = new float[cols + r]; for (int c = 0; c < g[r].Length; c++) g[r][c] = r * 100 + c; }
        return g;
    }
    static double JaggedSum(float[][] g) { double s = 0; for (int r = 0; r < g.Length; r++) for (int c = 0; c < g[r].Length; c++) s += g[r][c]; return s; }

    static Cell[] Cells(int n) {
        Cell[] cs = new Cell[n];
        for (int i = 0; i < n; i++) { cs[i].X = i; cs[i].Y = i * 0.5f; cs[i].Link = new Node(i * 3, i > 0 ? cs[i - 1].Link : null); }
        return cs;
    }
    static long CellsSum(Cell[] cs) { long s = 0; for (int i = 0; i < cs.Length; i++) s += cs[i].X + (long)cs[i].Y + SumList(cs[i].Link); return s; }

    static int MakeBad(Node n) { Bad b = new Bad(n); return b.V; }
    static int Repeated(int times, bool fail) { int s = 0; for (int i = 0; i < times; i++) s += MakeBad(fail ? null : new Node(i, null)); return s; }

    static string Try(Func<int> f) { try { return f().ToString(); } catch (NullReferenceException) { return "NRE"; } }

    static void Main() {
        Node[] keep = Churn(40, 3000);
        Console.WriteLine("churn " + keep.Length + " " + SumKept(keep));
        GC.Collect();
        Console.WriteLine("after gc " + SumKept(keep));
        Node[] keep2 = Churn(25, 1500);                        // allocates more, over the survivors of the first
        GC.Collect();
        Console.WriteLine("again " + SumKept(keep) + " " + SumKept(keep2));
        Console.WriteLine("hold " + Hold(3000) + " " + Hold(1));
        float[][] g = Jagged(60, 40); GC.Collect(); Console.WriteLine("jagged " + JaggedSum(g) + " " + g.Length + " " + g[59].Length);
        Cell[] cs = Cells(500); GC.Collect(); Console.WriteLine("cells " + CellsSum(cs) + " " + cs[499].Link.Value + " " + Length(cs[499].Link));
        string line = "bad";
        for (int i = 0; i < 4; i++) line += " " + Try(() => Repeated(100, false)) + " " + Try(() => Repeated(100, true)) + " " + Try(() => MakeBad(null));
        Console.WriteLine(line);
        // after all those exceptions allocation and collection must still work
        Node[] keep3 = Churn(20, 2000); GC.Collect();
        Console.WriteLine("last " + SumKept(keep3) + " " + SumKept(keep) + " " + SumKept(keep2));
    }
}
