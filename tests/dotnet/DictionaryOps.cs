using System;
using System.Collections;
using System.Collections.Generic;
using System.Text;

// Dictionary<K,V>, compared with Mono line for line. Its Count was one too low after the first resize, and it was
// quadratic in the number of items (partly the collector's headroom cap, partly two Lists per slot).
struct Pt : IEquatable<Pt> {
    public int X, Y;
    public Pt(int x, int y) { X = x; Y = y; }
    public bool Equals(Pt o) { return X == o.X && Y == o.Y; }
    public override bool Equals(object o) { return o is Pt && Equals((Pt)o); }
    public override int GetHashCode() { return X * 31 + Y; }
    public override string ToString() { return "(" + X + "," + Y + ")"; }
}

class AllSameHash : IEqualityComparer<string> {      // every key collides: one long chain
    public bool Equals(string a, string b) { return a == b; }
    public int GetHashCode(string s) { return 7; }
}

class CaseInsensitive : IEqualityComparer<string> {
    public bool Equals(string a, string b) { return a.ToLower() == b.ToLower(); }
    public int GetHashCode(string s) { return s.ToLower().GetHashCode(); }
}

class Program {
    static string Show<K, V>(Dictionary<K, V> d) {
        StringBuilder sb = new StringBuilder();
        foreach (KeyValuePair<K, V> kv in d) { sb.Append(kv.Key); sb.Append('='); sb.Append(kv.Value); sb.Append(' '); }
        return sb.ToString() + "[" + d.Count + "]";
    }

    static string Try(Action a) {
        try { a(); return "no exception"; } catch (Exception e) { return e.GetType().Name; }
    }

    static void Main() {
        // growth through many resizes: Count must be exact at every size, and every key found
        Dictionary<int, int> big = new Dictionary<int, int>();
        bool countsOk = true; long sum = 0; int missing = 0;
        for (int i = 0; i < 5000; i++) { big[i * 7] = i; if (big.Count != i + 1) countsOk = false; }
        for (int i = 0; i < 5000; i++) { int v; if (big.TryGetValue(i * 7, out v)) sum += v; else missing++; }
        Console.WriteLine("grow: counts exact " + countsOk + ", count " + big.Count + ", sum " + sum + ", missing " + missing);

        // overwrite does not change Count
        big[7] = 999; big[7] = 1000;
        Console.WriteLine("overwrite: " + big[7] + " count " + big.Count);

        // enumeration order is insertion order; removing and adding reuses the freed slot
        Dictionary<string, int> d = new Dictionary<string, int>();
        d.Add("one", 1); d.Add("two", 2); d.Add("three", 3); d.Add("four", 4); d.Add("five", 5);
        Console.WriteLine("insert order: " + Show(d));
        d.Remove("two"); d.Remove("four");
        Console.WriteLine("after removing two, four: " + Show(d));
        d.Add("six", 6); d.Add("seven", 7); d.Add("eight", 8);
        Console.WriteLine("after adding six, seven, eight: " + Show(d));
        d.Remove("one"); d.Add("nine", 9);
        Console.WriteLine("remove one, add nine: " + Show(d));

        // Keys and Values follow the same order
        StringBuilder sb = new StringBuilder();
        foreach (string k in d.Keys) sb.Append(k + " ");
        foreach (int v in d.Values) sb.Append(v + " ");
        Console.WriteLine("keys/values: " + sb);

        // Remove, TryGetValue, ContainsKey, ContainsValue
        Console.WriteLine("remove present " + d.Remove("three") + ", remove absent " + d.Remove("three"));
        int got; bool found = d.TryGetValue("five", out got);
        Console.WriteLine("TryGetValue present " + found + " " + got);
        found = d.TryGetValue("nope", out got);
        Console.WriteLine("TryGetValue absent " + found + " " + got);
        Console.WriteLine("ContainsKey " + d.ContainsKey("five") + " " + d.ContainsKey("two"));
        Console.WriteLine("ContainsValue " + d.ContainsValue(5) + " " + d.ContainsValue(2));
        Dictionary<string, string> withNull = new Dictionary<string, string>();
        withNull["a"] = null; withNull["b"] = "x";
        Console.WriteLine("ContainsValue null " + withNull.ContainsValue(null) + ", of x " + withNull.ContainsValue("x") + ", of y " + withNull.ContainsValue("y"));

        // Clear, then reuse
        d.Clear();
        Console.WriteLine("cleared: " + Show(d));
        d.Add("again", 1);
        Console.WriteLine("reused: " + Show(d));

        // exceptions (by type)
        Dictionary<string, int> e = new Dictionary<string, int>();
        e.Add("k", 1);
        Console.WriteLine("null key add:  " + Try(() => e.Add(null, 1)));
        Console.WriteLine("null key get:  " + Try(() => { int x = e[null]; }));
        Console.WriteLine("null key set:  " + Try(() => { e[null] = 1; }));
        Console.WriteLine("null contains: " + Try(() => e.ContainsKey(null)));
        Console.WriteLine("null remove:   " + Try(() => e.Remove(null)));
        Console.WriteLine("duplicate add: " + Try(() => e.Add("k", 2)));
        Console.WriteLine("missing key:   " + Try(() => { int x = e["zzz"]; }));
        Console.WriteLine("negative cap:  " + Try(() => new Dictionary<int, int>(-1)));
        Console.WriteLine("null source:   " + Try(() => new Dictionary<int, int>((IDictionary<int, int>)null)));

        // modifying while enumerating
        Dictionary<int, int> m = new Dictionary<int, int>();
        for (int i = 0; i < 5; i++) m[i] = i;
        Console.WriteLine("add while enumerating:    " + Try(() => { foreach (KeyValuePair<int, int> kv in m) m[100 + kv.Key] = 0; }));
        Console.WriteLine("remove while enumerating: " + Try(() => { foreach (KeyValuePair<int, int> kv in m) m.Remove(kv.Key); }));

        // collisions: one chain, remove from its head, middle and tail
        Dictionary<string, int> c = new Dictionary<string, int>(new AllSameHash());
        for (int i = 0; i < 40; i++) c["k" + i] = i;
        c.Remove("k39"); c.Remove("k20"); c.Remove("k0");
        int found2 = 0; long csum = 0;
        for (int i = 0; i < 40; i++) { int v; if (c.TryGetValue("k" + i, out v)) { found2++; csum += v; } }
        Console.WriteLine("collisions: count " + c.Count + " found " + found2 + " sum " + csum);
        c["k20"] = 20;
        Console.WriteLine("collisions: re-added k20, count " + c.Count + " " + c["k20"]);

        // a comparer that ignores case; struct keys; negative and extreme int keys
        Dictionary<string, int> ci = new Dictionary<string, int>(new CaseInsensitive());
        ci["Hello"] = 1; ci["HELLO"] = 2; ci["world"] = 3;
        Console.WriteLine("case-insensitive: count " + ci.Count + " " + ci["hello"] + " " + ci.ContainsKey("WORLD"));
        Dictionary<Pt, string> pts = new Dictionary<Pt, string>();
        for (int i = 0; i < 50; i++) pts[new Pt(i, -i)] = "p" + i;
        Console.WriteLine("struct keys: count " + pts.Count + " " + pts[new Pt(7, -7)] + " " + pts.ContainsKey(new Pt(7, 7)));
        Dictionary<int, string> ints = new Dictionary<int, string>();
        ints[-1] = "m1"; ints[int.MinValue] = "min"; ints[int.MaxValue] = "max"; ints[0] = "zero"; ints[-12345] = "neg";
        Console.WriteLine("extreme keys: " + Show(ints) + " " + ints[int.MinValue] + " " + ints[int.MaxValue]);

        // construct from another dictionary
        Dictionary<string, int> copy = new Dictionary<string, int>(e);
        e["extra"] = 5;
        Console.WriteLine("copy: " + Show(copy) + " original " + Show(e));

        // CopyTo
        Dictionary<int, string> src = new Dictionary<int, string>();
        src[3] = "c"; src[1] = "a"; src[2] = "b";
        int[] ka = new int[5]; string[] va = new string[4];
        src.Keys.CopyTo(ka, 1); src.Values.CopyTo(va, 1);
        KeyValuePair<int, string>[] pa = new KeyValuePair<int, string>[3];
        ((ICollection<KeyValuePair<int, string>>)src).CopyTo(pa, 0);
        Console.WriteLine("CopyTo: keys " + ka[0] + ka[1] + ka[2] + ka[3] + ka[4] + " values " + va[1] + va[2] + va[3] + " pairs " + pa[0].Key + pa[1].Key + pa[2].Key);
        Console.WriteLine("CopyTo too small: " + Try(() => src.Keys.CopyTo(new int[2], 0)));

        // ICollection<KeyValuePair>: Contains and Remove check the value too
        ICollection<KeyValuePair<int, string>> col = src;
        Console.WriteLine("Contains (1,a) " + col.Contains(new KeyValuePair<int, string>(1, "a")) + ", (1,x) " + col.Contains(new KeyValuePair<int, string>(1, "x")));
        Console.WriteLine("Remove (1,x) " + col.Remove(new KeyValuePair<int, string>(1, "x")) + ", (1,a) " + col.Remove(new KeyValuePair<int, string>(1, "a")) + " -> " + Show(src));

        // the non-generic IDictionary
        IDictionary nd = new Dictionary<string, int>();
        nd["x"] = 10; nd.Add("y", 20);
        Console.WriteLine("IDictionary: " + nd["x"] + " " + nd["y"] + " missing " + (nd["zzz"] == null) + " wrong type " + (nd[5] == null) + " contains " + nd.Contains("x") + nd.Contains("q") + " count " + nd.Count);
        StringBuilder nb = new StringBuilder();
        IDictionaryEnumerator en = nd.GetEnumerator();
        while (en.MoveNext()) nb.Append(en.Key + "=" + en.Value + " ");
        Console.WriteLine("IDictionary enumerate: " + nb);
        nd.Remove("x");
        Console.WriteLine("IDictionary remove: count " + nd.Count);

        // heavy churn: add, remove and re-add in patterns that exercise the free list
        Dictionary<int, int> churn = new Dictionary<int, int>();
        long acc = 0;
        for (int round = 0; round < 20; round++) {
            for (int i = 0; i < 300; i++) churn[round * 1000 + i] = i;
            for (int i = 0; i < 300; i += 3) churn.Remove(round * 1000 + i);
            if (round > 3) for (int i = 0; i < 300; i += 2) churn.Remove((round - 3) * 1000 + i);
        }
        foreach (KeyValuePair<int, int> kv in churn) acc = acc * 31 + kv.Key + kv.Value;
        Console.WriteLine("churn: count " + churn.Count + " checksum " + acc);
    }
}
