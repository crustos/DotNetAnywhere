using System;
using System.Collections.Generic;

// What the garbage collector must keep alive, checked by using it afterwards (a freed object's memory is reused by the churn, so a
// wrongly collected object shows up as a wrong type or value): references held only inside arrays of structs and inside struct fields,
// zero-sized objects (an `object` has no fields: its memory start is its end), interior pointers into arrays held across collections,
// dictionaries and lists growing under allocation pressure, objects reachable only from a local or from the evaluation stack, and weak
// references to objects that are, and are not, kept. Compared with Mono line for line.
struct Slot { public int a; public object o; public long k; public string s; }
class Node { public Node next; public int v; public object payload; }
class Marker { }
class Program {
    static uint h;
    static void Mix(uint v) { h = (h ^ v) * 16777619u; }
    static void MixL(long v) { Mix((uint)v); Mix((uint)(v >> 32)); }
    static void Group(string name) { Console.WriteLine(name + " " + h); h = 2166136261u; }

    static object sink;
    // allocate a lot of things of many shapes, collecting several times, so that freed memory is reused
    static void Churn(int rounds) {
        for (int r = 0; r < rounds; r++) {
            for (int i = 0; i < 400; i++) {
                switch (i % 5) {
                case 0: sink = new int[(i % 17) + 1]; break;
                case 1: sink = new Node(); break;
                case 2: sink = "churn" + i + r; break;
                case 3: sink = new Slot[(i % 7) + 1]; break;
                default: sink = new Marker(); break;
                }
            }
            GC.Collect();
        }
        sink = null;
    }
    static void BumpInside(ref int x, int times) { for (int i = 0; i < times; i++) { Churn(1); x += i + 1; } }
    static void BumpLong(ref long x) { Churn(2); x = x * 3 + 1; }

    static Node Chain(int n) { Node head = null; for (int i = 0; i < n; i++) { Node nd = new Node(); nd.v = i * 7; nd.next = head; nd.payload = "p" + i; head = nd; } return head; }
    static long Sum(Node n) { long s = 0; while (n != null) { s = s * 31 + n.v + ((string)n.payload).Length; n = n.next; } return s; }

    public static void Main() {
        h = 2166136261u;
        // references that exist only inside an array of structs
        Slot[] slots = new Slot[200];
        for (int i = 0; i < slots.Length; i++) { slots[i].a = i; slots[i].o = (i % 3 == 0) ? (object)new Marker() : (object)("s" + i); slots[i].k = (long)i << 36; slots[i].s = "str" + i * 5; }
        Churn(4);
        for (int i = 0; i < slots.Length; i++) {
            Mix((uint)slots[i].a); MixL(slots[i].k); Mix((uint)slots[i].s.Length); Mix((uint)(slots[i].s == "str" + i * 5 ? 1 : 0));
            Mix((slots[i].o is Marker) ? 1u : (uint)((string)slots[i].o).Length);
        }
        Group("arrays of structs");
        // zero-sized objects, kept only by an array of objects: each must still be an object, and distinct
        object[] objs = new object[150];
        for (int i = 0; i < objs.Length; i++) { objs[i] = new object(); }
        Churn(4);
        int distinct = 0, right = 0;
        for (int i = 0; i < objs.Length; i++) {
            if (objs[i].GetType() == typeof(object)) right++;
            bool same = false; for (int j = 0; j < i; j++) { if (objs[j] == objs[i]) same = true; } if (!same) distinct++;
        }
        Mix((uint)right); Mix((uint)distinct);
        Group("zero-sized objects");
        // a managed pointer into the middle of an array, held while the collector runs
        int[] data = new int[40]; long[] ldata = new long[10];
        for (int i = 0; i < data.Length; i++) data[i] = i * 3; for (int i = 0; i < ldata.Length; i++) ldata[i] = 1000 + i;
        BumpInside(ref data[17], 6); BumpInside(ref data[0], 3); BumpInside(ref data[39], 3); BumpLong(ref ldata[4]); BumpLong(ref ldata[9]);
        for (int i = 0; i < data.Length; i++) Mix((uint)data[i]); for (int i = 0; i < ldata.Length; i++) MixL(ldata[i]);
        Group("interior pointers");
        // linked structures, reachable only from a local, and growing collections under pressure
        Node chain = Chain(300);
        List<object> list = new List<object>(); Dictionary<int, string> dict = new Dictionary<int, string>(); Dictionary<string, Node> byName = new Dictionary<string, Node>();
        for (int i = 0; i < 1500; i++) {
            list.Add(i % 4 == 0 ? (object)new Node() : (object)("item" + i)); dict[i * 11] = "v" + i; if (i % 10 == 0) byName["n" + i] = Chain(3);
            if (i % 250 == 0) { Churn(1); }
        }
        Churn(3);
        MixL(Sum(chain)); Mix((uint)list.Count); Mix((uint)dict.Count);
        long acc = 0; for (int i = 0; i < 1500; i++) { string v; if (dict.TryGetValue(i * 11, out v)) acc = acc * 7 + v.Length; object o = list[i]; acc += (o is Node) ? 1 : ((string)o).Length; }
        MixL(acc);
        for (int i = 0; i < 1500; i += 10) { Node n; if (byName.TryGetValue("n" + i, out n)) MixL(Sum(n)); }
        Group("linked structures and collections");
        // objects reachable only from the evaluation stack while the collector runs
        long ev = Sum(Chain(50)) + Sum(MakeAndChurn()) + (long)MakeAndChurn2().Length;
        MixL(ev);
        Group("evaluation stack");
        // weak references: alive while something holds the target
        Marker held = new Marker(); WeakReference wk = new WeakReference(held); WeakReference wl = new WeakReference(new object[3]);
        Churn(3);
        Mix(wk.IsAlive ? 1u : 0u); Mix(wk.Target == held ? 1u : 0u); Mix(held.GetType() == typeof(Marker) ? 1u : 0u);
        Group("weak references");
    }
    static Node MakeAndChurn() { Node n = Chain(40); Churn(2); return n; }
    static string MakeAndChurn2() { string s = "keep" + 12345; Churn(2); return s + s; }
}
