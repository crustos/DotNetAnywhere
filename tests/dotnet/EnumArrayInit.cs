// Array initializers of every small element type, including enums over byte/sbyte/short/ushort (DNA read the compiler's data at the wrong size).
using System;
enum B8 : byte { A = 200, B = 3 }
enum S8 : sbyte { A = -3, B = 100 }
enum I16 : short { A = -300, B = 300 }
enum U16 : ushort { A = 65000, B = 3 }
enum I32 : int { A = -5, B = 7 }
class EnumArrayInit { static void Main() {
    bool[] b = { true, false, true, true };           Console.WriteLine("bool " + b[0] + b[1] + b[2] + b[3] + " " + b.Length);
    char[] c = { 'x', 'y', 'z' };                    Console.WriteLine("char " + c[0] + c[1] + c[2]);
    short[] sh = { -2, 300, -4000 };                 Console.WriteLine("short " + sh[0] + " " + sh[1] + " " + sh[2]);
    B8[] e1 = { B8.A, B8.B, B8.A };                  Console.WriteLine("b8 " + (int)e1[0] + " " + (int)e1[1] + " " + (int)e1[2]);
    S8[] e2 = { S8.A, S8.B, S8.A };                  Console.WriteLine("s8 " + (int)e2[0] + " " + (int)e2[1] + " " + (int)e2[2]);
    I16[] e3 = { I16.A, I16.B, I16.A };              Console.WriteLine("i16 " + (int)e3[0] + " " + (int)e3[1] + " " + (int)e3[2]);
    U16[] e4 = { U16.A, U16.B, U16.A };              Console.WriteLine("u16 " + (int)e4[0] + " " + (int)e4[1] + " " + (int)e4[2]);
    I32[] e5 = { I32.A, I32.B, I32.A };              Console.WriteLine("i32 " + (int)e5[0] + " " + (int)e5[1] + " " + (int)e5[2]);
    long[] l = { -1L, 1L << 40, 5L };                Console.WriteLine("long " + l[0] + " " + l[1] + " " + l[2]);
    double[] d = { 1.5, -2.25 };                     Console.WriteLine("double " + d[0] + " " + d[1]);
} }
