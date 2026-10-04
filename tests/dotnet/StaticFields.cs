using System;

// Reading and writing static fields of every kind. Reading a `static long` used to crash the JIT ("Opcode not
// available"): the load-static-field opcode for 64-bit integers had no handler. The output is compared with Mono's.
struct Pair { public int A; public long B; }

class Statics {
    public static long L = 5000000000L;
    public static ulong UL = 18000000000000000000UL;
    public static int I = -7;
    public static uint UI = 4000000000u;
    public static short S = -300;
    public static ushort US = 60000;
    public static sbyte SB = -5;
    public static byte B = 200;
    public static char C = 'q';
    public static bool Flag = true;
    public static float F = 1.25f;
    public static double D = -2.5e10;
    public static string Str = "hello";
    public static object Obj = "boxed";
    public static Pair PairValue = new Pair { A = 3, B = 4000000000L };
    public static long[] Arr = new long[] { 1L, 2L, 3000000000L };
    public static readonly long ReadOnlyLong = 1L << 40;
    public static long Counter;
}

class Program {
    static void Main() {
        Console.WriteLine("long " + Statics.L + " ulong " + Statics.UL);
        Console.WriteLine("int " + Statics.I + " uint " + Statics.UI);
        Console.WriteLine("short " + Statics.S + " ushort " + Statics.US + " sbyte " + Statics.SB + " byte " + Statics.B);
        Console.WriteLine("char " + Statics.C + " bool " + Statics.Flag);
        Console.WriteLine("float " + (Statics.F * 2) + " double " + (Statics.D / 2));
        Console.WriteLine("string " + Statics.Str + " object " + Statics.Obj);
        Console.WriteLine("struct " + Statics.PairValue.A + " " + Statics.PairValue.B);
        Console.WriteLine("array " + Statics.Arr[0] + " " + Statics.Arr[2] + " readonly " + Statics.ReadOnlyLong);

        // writes, then reads back
        Statics.L = Statics.L * 3 + 1;
        Statics.UL = Statics.UL / 2;
        Statics.D = Statics.D + 0.5;
        Statics.F = Statics.F + Statics.F;
        Statics.PairValue.B = Statics.L - 1;
        Console.WriteLine("after: " + Statics.L + " " + Statics.UL + " " + Statics.D + " " + Statics.F + " " + Statics.PairValue.B);

        long total = 0;
        for (int i = 0; i < 1000; i++) { Statics.Counter += i; total += Statics.Counter; }
        Console.WriteLine("counter " + Statics.Counter + " total " + total);
        Console.WriteLine("compare " + (Statics.L > 10000000000L) + " " + (Statics.Counter == 499500L) + " " + (Statics.ReadOnlyLong == (1L << 40)));
    }
}
