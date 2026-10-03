using System;
using System.Collections.Generic;

// List<T> must reject out-of-range indices and must only ever look at live elements.
class Program {
    static bool Throws(Action a) {
        try { a(); return false; } catch (ArgumentOutOfRangeException) { return true; }
    }

    public static int Main() {
        var l = new List<int>(); l.Add(1); l.Add(2);

        // Remove only searches live elements: 0 is not present, even though the backing
        // array has default(int) slots past Count.
        if (l.Remove(0)) return 1;
        if (l.Count != 2 || l[0] != 1 || l[1] != 2) return 2;
        if (!l.Remove(2) || l.Count != 1) return 3;
        if (l.Remove(2)) return 4;                       // already gone

        // RemoveAt / Insert / indexer bounds
        if (!Throws(delegate { l.RemoveAt(5); })) return 5;
        if (!Throws(delegate { l.RemoveAt(-1); })) return 6;
        if (!Throws(delegate { l.RemoveAt(l.Count); })) return 7;
        if (!Throws(delegate { l.Insert(5, 9); })) return 8;
        if (!Throws(delegate { int x = l[1]; })) return 9;
        if (l.Count != 1 || l[0] != 1) return 10;        // failed calls changed nothing

        // valid edge cases still work
        l.Insert(1, 7);                                  // insert at Count
        l.Insert(0, 5);
        if (l.Count != 3 || l[0] != 5 || l[1] != 1 || l[2] != 7) return 11;
        l.RemoveAt(2); l.RemoveAt(0);
        if (l.Count != 1 || l[0] != 1) return 12;
        l.RemoveAt(0);
        if (l.Count != 0) return 13;
        if (!Throws(delegate { l.RemoveAt(0); })) return 14;   // empty list
        return 0;
    }
}
