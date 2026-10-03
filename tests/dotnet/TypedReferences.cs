using System;

// __makeref / __refvalue / __reftype compile to mkrefany / refanyval / refanytype. A
// TypedReference is an address plus the type it was made for; reading it back as another type
// throws InvalidCastException.
struct S { public int a; public long b; }
class Holder { public int field; public string text; }

class Program {
    static void SetInt(TypedReference r, int v) { __refvalue(r, int) = v; }
    static int GetInt(TypedReference r) { return __refvalue(r, int); }
    static Type TypeOf(TypedReference r) { return __reftype(r); }

    public static int Main() {
        // a local, read and written through the reference
        int x = 5;
        TypedReference r = __makeref(x);
        if (__refvalue(r, int) != 5) return 1;
        __refvalue(r, int) = 9;
        if (x != 9) return 2;
        if (TypeOf(r) != typeof(int)) return 3;

        // passed to other methods
        SetInt(__makeref(x), 42);
        if (x != 42 || GetInt(__makeref(x)) != 42) return 4;

        // other types: reference, long, double
        string s = "a"; TypedReference rs = __makeref(s);
        __refvalue(rs, string) = "b";
        if (s != "b" || TypeOf(rs) != typeof(string)) return 5;
        long l = 1L << 40; TypedReference rl = __makeref(l);
        __refvalue(rl, long) += 1;
        if (l != (1L << 40) + 1 || TypeOf(rl) != typeof(long)) return 6;
        double d = 2.5; TypedReference rd = __makeref(d);
        __refvalue(rd, double) *= 2;
        if (d != 5.0) return 7;

        // fields and array elements
        S sv = new S(); sv.a = 3; sv.b = 4;
        TypedReference ra = __makeref(sv.a), rb = __makeref(sv.b);
        __refvalue(ra, int) = 30; __refvalue(rb, long) = 40;
        if (sv.a != 30 || sv.b != 40) return 8;
        Holder h = new Holder();
        SetInt(__makeref(h.field), 11);
        if (h.field != 11) return 9;
        int[] arr = new int[3];
        SetInt(__makeref(arr[1]), 77);
        if (arr[0] != 0 || arr[1] != 77 || arr[2] != 0) return 10;

        // the wrong type is refused
        bool threw = false;
        try { long bad = __refvalue(r, long); } catch (InvalidCastException) { threw = true; }
        if (!threw) return 11;
        threw = false;
        try { string bad = __refvalue(rl, string); } catch (InvalidCastException) { threw = true; }
        if (!threw) return 12;

        // a reference made for an enum or a struct keeps that type
        TypedReference rst = __makeref(sv);
        if (TypeOf(rst) != typeof(S)) return 13;
        if (__refvalue(rst, S).a != 30) return 14;
        return 0;
    }
}
