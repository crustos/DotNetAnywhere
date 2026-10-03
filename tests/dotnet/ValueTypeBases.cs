using System;
using System.Collections.Generic;

// System.ValueType and System.Enum are classes: variables, fields, parameters and collection
// elements declared with those types hold a reference (and may be null), even though the structs
// and enums derived from them are values. DNA treated both as zero-sized value types, so copying
// one moved nothing and using it crashed. The output is compared with Mono's, line for line.
enum Color : byte { Red = 1, Green = 2 }
enum Big : int { Seven = 7 }
struct Pt { public int X, Y; public override string ToString() { return "(" + X + "," + Y + ")"; } }
struct Tiny { public byte B; }

class Holder {
    public ValueType vt;
    public Enum en;
    public ValueType[] vts = new ValueType[3];
}

class Program {
    static string Describe(ValueType v) { return v == null ? "null" : v.GetType().Name + "=" + v; }
    static string Describe(Enum e) { return e == null ? "null" : e.GetType().Name + "=" + e; }
    static ValueType Pass(ValueType v) { return v; }
    static Enum PassE(Enum e) { return e; }

    static void Main() {
        // locals holding a struct, a primitive and an enum through ValueType / Enum
        ValueType a = new Pt { X = 3, Y = 4 };
        ValueType b = 42;
        ValueType c = Color.Green;
        ValueType d = 2.5;
        ValueType n = null;
        Console.WriteLine(Describe(a) + " " + Describe(b) + " " + Describe(c) + " " + Describe(d) + " " + Describe(n));
        Console.WriteLine((a == null) + " " + (n == null) + " " + (b != null));

        Enum e = Color.Red, e2 = Big.Seven, e3 = null;
        Console.WriteLine(Describe(e) + " " + Describe(e2) + " " + Describe(e3));
        Console.WriteLine((e == null) + " " + (e3 == null) + " " + e.Equals(Color.Red) + " " + e.Equals(Color.Green) + " " + e.Equals(Big.Seven));

        // passed and returned
        Console.WriteLine(Describe(Pass(a)) + " " + Describe(Pass(b)) + " " + Describe(Pass(null)));
        Console.WriteLine(Describe(PassE(Color.Green)) + " " + Describe(PassE(null)));

        // fields and arrays of them
        Holder h = new Holder();
        h.vt = new Tiny { B = 9 }; h.en = Big.Seven;
        h.vts[0] = 1; h.vts[1] = Color.Red; h.vts[2] = new Pt { X = 1, Y = 2 };
        Console.WriteLine(Describe(h.vt) + " " + Describe(h.en));
        Console.WriteLine(Describe(h.vts[0]) + " " + Describe(h.vts[1]) + " " + Describe(h.vts[2]));
        h.vt = null; h.en = null;
        Console.WriteLine((h.vt == null) + " " + (h.en == null));

        // collections of them
        List<ValueType> list = new List<ValueType>();
        list.Add(5); list.Add(new Pt { X = 7, Y = 8 }); list.Add(Color.Green); list.Add(null);
        string s = "";
        for (int i = 0; i < list.Count; i++) { s += Describe(list[i]) + ";"; }
        Console.WriteLine(s);
        List<Enum> elist = new List<Enum>();
        elist.Add(Color.Red); elist.Add(Big.Seven);
        Console.WriteLine(Describe(elist[0]) + " " + Describe(elist[1]) + " " + elist.Count);

        // casts back to the concrete type
        Console.WriteLine(((Pt)a).X + " " + (int)b + " " + (Color)c + " " + (double)d);
        Console.WriteLine((a is Pt) + " " + (a is Tiny) + " " + (c is Enum) + " " + (c is Color) + " " + (b is ValueType) + " " + (e is Color));
        // is/as with the base types
        object o = Color.Green;
        Console.WriteLine(((o as Enum) != null) + " " + ((o as ValueType) != null) + " " + (((object)"x" as ValueType) == null));
        // equality and hashing through the base type
        Console.WriteLine(a.Equals(new Pt { X = 3, Y = 4 }) + " " + a.Equals(new Pt { X = 3, Y = 5 }) + " " + b.Equals(42) + " " + b.Equals(43));
        Console.WriteLine((a.GetHashCode() == new Pt { X = 3, Y = 4 }.GetHashCode()) + " " + (b.GetHashCode() == 42.GetHashCode()));
    }
}
