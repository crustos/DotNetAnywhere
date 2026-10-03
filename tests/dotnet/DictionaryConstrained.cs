using System.Collections.Generic;

// Dictionary<int,int> compiles to `constrained.` callvirts (GetHashCode/Equals on
// a generic key). The JIT reaches those through a goto that used to skip the
// initialisation of a local, so this crashed or not depending on stack garbage.
class DictionaryConstrained {
    static int Main() {
        var d = new Dictionary<int, int>();
        d.Add(1, 10);
        d[2] = 20;
        d[2] += 1;
        int c = d.Count;
        bool k = d.ContainsKey(2);
        d.Remove(1);
        return (c == 2 && k && d[2] == 21 && d.Count == 1) ? 0 : 1;
    }
}
