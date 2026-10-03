using System;

// Field access through a null reference must throw NullReferenceException (DNA used to
// dereference address 0 and segfault on stores, and read garbage on loads).
struct V { public int A; public long B; }
class Inner { public int X; }
class Holder {
    public int I; public long L; public double D; public float F; public byte Bt; public short S;
    public object O; public string Str; public Inner In; public V Val;
}

class Program {
    static void Bump(ref int x) { x++; }
    static Holder Null() { return null; }
    static int Deep(Holder h) { return Mid(h); }
    static int Mid(Holder h) { return h.I; }              // throws two frames below the catch

    static bool Npe(Action a) {
        try { a(); return false; } catch (NullReferenceException) { return true; }
    }

    public static int Main() {
        Holder h = Null();
        int sink = 0;

        // stores
        if (!Npe(delegate { h.I = 1; })) return 1;              // int
        if (!Npe(delegate { h.L = 1L; })) return 2;             // long
        if (!Npe(delegate { h.D = 1.5; })) return 3;            // double
        if (!Npe(delegate { h.F = 1f; })) return 4;             // float
        if (!Npe(delegate { h.Bt = 1; })) return 5;             // byte
        if (!Npe(delegate { h.S = 1; })) return 6;              // short
        if (!Npe(delegate { h.O = "x"; })) return 7;            // object
        if (!Npe(delegate { h.Val = new V(); })) return 8;      // struct
        if (!Npe(delegate { h.Val.A = 3; })) return 9;          // field of a struct field

        // loads
        if (!Npe(delegate { sink = h.I; })) return 10;
        if (!Npe(delegate { long l = h.L; })) return 11;
        if (!Npe(delegate { double d = h.D; })) return 12;
        if (!Npe(delegate { object o = h.O; })) return 13;
        if (!Npe(delegate { V v = h.Val; })) return 14;
        if (!Npe(delegate { int a = h.Val.A; })) return 15;

        // a field of a null *field*: the case Crust's tests hit (a.b.x = 5 with b never created)
        Holder real = new Holder();
        if (!Npe(delegate { real.In.X = 5; })) return 16;
        if (!Npe(delegate { sink = real.In.X; })) return 17;

        // address of a field of null (ldflda)
        if (!Npe(delegate { Bump(ref h.I); })) return 18;

        // thrown several frames below the catch
        bool caught = false;
        try { Deep(h); } catch (NullReferenceException) { caught = true; }
        if (!caught) return 19;

        // and with a finally in between
        int fin = 0;
        try { try { sink = h.I; } finally { fin = 1; } } catch (NullReferenceException) { fin += 10; }
        if (fin != 11) return 20;

        // non-null access is unaffected, including the boundary values
        real.I = 7; real.L = 1L << 40; real.D = 2.5; real.Bt = 200; real.S = -3; real.O = "o";
        real.In = new Inner(); real.In.X = 9; real.Val.A = 4; real.Val.B = 5L;
        Bump(ref real.I);
        if (real.I != 8 || real.L != (1L << 40) || real.D != 2.5 || real.Bt != 200 || real.S != -3) return 21;
        if ((string)real.O != "o" || real.In.X != 9 || real.Val.A != 4 || real.Val.B != 5L) return 22;
        return 0;
    }
}
