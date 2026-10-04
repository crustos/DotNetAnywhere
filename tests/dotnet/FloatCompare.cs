using System;
// Float and double comparisons with NaN. The unordered branches (bge.un, bgt.un, ble.un, blt.un: what C# compiles
// `if (a < b) body` to, to branch around the body) shared a handler with the ordered ones, so with a NaN operand every
// ordered comparison came out TRUE on DNA. Compared with Mono line for line.
public class P {
    static string F(float a, float b) {
        string r = "";
        if (a < b) r += "lt "; else r += "-- ";
        if (a <= b) r += "le "; else r += "-- ";
        if (a > b) r += "gt "; else r += "-- ";
        if (a >= b) r += "ge "; else r += "-- ";
        if (a == b) r += "eq "; else r += "-- ";
        if (a != b) r += "ne "; else r += "-- ";
        if (!(a < b)) r += "!lt "; else r += "--- ";
        if (!(a >= b)) r += "!ge "; else r += "--- ";
        if (!(a > b)) r += "!gt "; else r += "--- ";
        if (!(a <= b)) r += "!le "; else r += "--- ";
        return r;
    }
    static string D(double a, double b) {
        string r = "";
        if (a < b) r += "lt "; else r += "-- ";
        if (a <= b) r += "le "; else r += "-- ";
        if (a > b) r += "gt "; else r += "-- ";
        if (a >= b) r += "ge "; else r += "-- ";
        if (a == b) r += "eq "; else r += "-- ";
        if (a != b) r += "ne "; else r += "-- ";
        if (!(a < b)) r += "!lt "; else r += "--- ";
        if (!(a >= b)) r += "!ge "; else r += "--- ";
        return r;
    }
    static string V(float a, float b) { bool x = a < b, y = a >= b, z = a > b, w = a <= b, e = a == b, n = a != b; return "" + x + y + z + w + e + n; }
    static string Logic(float a, float b) {
        return ((a < b && b < 10f) ? "A" : "B") + ((a > b || a == a) ? "C" : "D") + (a < b ? "E" : "F") + (a <= 1f ? "G" : "H") + (2.5f > b ? "I" : "J");
    }
    static int Loops() {
        float nan = float.NaN; int c = 0;
        for (float f = 0f; f < nan; f += 1f) { c++; if (c > 5) break; }                   // a NaN bound: no iterations
        for (float f = 0f; !(f >= nan); f += 1f) { c += 100; if (c > 500) break; }        // !(f >= NaN) is true: runs until the break
        double d = 0.0;
        while (d < double.NaN) { c += 1000; d += 1.0; if (c > 5000) break; }
        return c;
    }
    public static int Main() {
        float nan = float.NaN, one = 1f, two = 2f;
        Console.WriteLine("f nan,1: " + F(nan, one));
        Console.WriteLine("f 1,nan: " + F(one, nan));
        Console.WriteLine("f nan,nan: " + F(nan, nan));
        Console.WriteLine("f 1,2:   " + F(one, two));
        Console.WriteLine("f 2,1:   " + F(two, one));
        Console.WriteLine("f 1,1:   " + F(one, one));
        Console.WriteLine("d nan,1: " + D(double.NaN, 1.0));
        Console.WriteLine("d 1,nan: " + D(1.0, double.NaN));
        Console.WriteLine("d 1,2:   " + D(1.0, 2.0));
        Console.WriteLine("values nan,1: " + V(nan, one) + "  1,nan: " + V(one, nan) + "  1,2: " + V(one, two));
        Console.WriteLine("logic nan,1: " + Logic(nan, one) + "  1,nan: " + Logic(one, nan) + "  1,2: " + Logic(one, two) + "  3,2: " + Logic(3f, two));
        Console.WriteLine("loops: " + Loops());
        return 0;
    }
}
