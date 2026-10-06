// Attacks the allocator and the collector (native/src/Heap.c): objects of every size around the size-class boundaries and the limit between small
// and large objects, rolling windows of live objects whose contents are checked after forced collections (a slot freed too early, or handed out
// twice, shows up as a changed pattern), chunks that empty and are reused by other sizes, and linked structures that lose members at random.
// Every phase prints a checksum, so the output must equal Mono's.
using System;

class Node { public int V; public Node Next; public byte[] Pay; public Node(int v, Node n, int pay) { V = v; Next = n; Pay = new byte[pay]; for (int i = 0; i < pay; i++) Pay[i] = (byte)(v + i); } }

class HeapStress {
    static uint seed = 12345;
    static int Rnd(int n) { seed = seed * 1664525u + 1013904223u; return (int)((seed >> 8) % (uint)n); }

    static uint Fill(byte[] a, int tag) { uint h = 0; for (int i = 0; i < a.Length; i++) { a[i] = (byte)(tag * 31 + i); h = h * 33 + a[i]; } return h; }
    static uint Check(byte[] a, int tag) { uint h = 0; for (int i = 0; i < a.Length; i++) { if (a[i] != (byte)(tag * 31 + i)) return 0xdeadbeef; h = h * 33 + a[i]; } return h; }

    // every length from 0 to 5000: each in its own size class or in the large path; all kept, then all checked
    static uint AllSizes() {
        byte[][] keep = new byte[5001][]; uint h = 0;
        for (int n = 0; n <= 5000; n++) { keep[n] = new byte[n]; h ^= Fill(keep[n], n); }
        GC.Collect();
        for (int n = 0; n <= 5000; n++) h = h * 7 + Check(keep[n], n);
        return h;
    }
    // a rolling window: random sizes, from tiny to well over the large threshold, the oldest dropped as the newest is made
    static uint Rolling(int rounds, int window, int maxLen) {
        byte[][] w = new byte[window][]; int[] tag = new int[window]; uint h = 0;
        for (int r = 0; r < rounds; r++) {
            int slot = r % window;
            if (w[slot] != null) h = h * 3 + Check(w[slot], tag[slot]);
            w[slot] = new byte[Rnd(maxLen)]; tag[slot] = r; Fill(w[slot], r);
            if (r % 1500 == 1499) { GC.Collect(); for (int i = 0; i < window; i++) if (w[i] != null) h = h * 3 + Check(w[i], tag[i]); }
        }
        return h;
    }
    // many objects with references to each other; random ones are dropped, the rest must stay intact
    static uint Linked(int n) {
        Node[] all = new Node[n]; Node head = null; uint h = 0;
        for (int i = 0; i < n; i++) { head = new Node(i, head, Rnd(40)); all[i] = head; }
        for (int i = 0; i < n; i++) if (Rnd(3) != 0) all[i] = null;
        GC.Collect();
        for (int i = 0; i < n; i++) { Node x = all[i]; if (x != null) { if (x.Pay.Length > 0 && x.Pay[0] != (byte)x.V) return 0xbad; h = h * 5 + (uint)x.V + (uint)x.Pay.Length; } }
        for (Node p = head; p != null; p = p.Next) h += (uint)p.V;      // the chain reaches everything, so nothing was freed
        return h;
    }
    // sizes change over time: the chunks of one size empty out and are reused for other sizes
    static uint Phases() {
        uint h = 0;
        for (int phase = 0; phase < 6; phase++) {
            int len = 8 << (phase * 2 % 9);
            object[] hold = new object[3000];
            for (int i = 0; i < hold.Length; i++) { byte[] b = new byte[len + Rnd(5)]; Fill(b, i + phase); hold[i] = b; }
            for (int i = 0; i < hold.Length; i += 2) hold[i] = null;
            GC.Collect();
            for (int i = 1; i < hold.Length; i += 2) h = h * 11 + Check((byte[])hold[i], i + phase);
        }
        return h;
    }
    static uint Tiny(int n) { uint h = 0; object o = null; for (int i = 0; i < n; i++) { o = new object(); h += (uint)(o == null ? 1 : 2); } int[][] small = new int[1000][]; for (int i = 0; i < n; i++) { small[i % 1000] = new int[1 + (i & 7)]; small[i % 1000][0] = i; } for (int i = 0; i < 1000; i++) if (small[i] != null) h = h * 3 + (uint)small[i][0]; return h; }

    // a collection while an exception is in flight: the finally block runs (and collects) after the throw and before the catch has the exception
    static string Unwind(int depth) {
        try {
            try {
                if (depth > 0) return Unwind(depth - 1);
                throw new InvalidOperationException("in flight " + (40 + depth));
            } finally {
                GC.Collect();
                byte[][] junk = new byte[60][]; for (int i = 0; i < junk.Length; i++) junk[i] = new byte[24 + i];
                GC.Collect();
            }
        } catch (InvalidOperationException e) { return e.GetType().Name + ":" + e.Message + ":" + depth; }
    }

    static void Main() {
        Console.WriteLine("sizes " + AllSizes());
        Console.WriteLine("rolling-small " + Rolling(8000, 120, 300));
        Console.WriteLine("rolling-mixed " + Rolling(3000, 60, 9000));
        Console.WriteLine("linked " + Linked(12000));
        Console.WriteLine("phases " + Phases());
        Console.WriteLine("tiny " + Tiny(40000));
        Console.WriteLine("unwind " + Unwind(0) + " " + Unwind(3));
        GC.Collect();
        Console.WriteLine("again " + AllSizes() + " " + Rolling(1500, 30, 9000) + " " + Linked(3000));
    }
}
