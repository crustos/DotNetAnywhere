using System;

// Array access must check: a null array is a NullReferenceException, an index outside 0..Length-1 an IndexOutOfRangeException.
// None of the element handlers used to (reading or writing past the end touched whatever was there, and a null array crashed
// the process). Every element type, the boundaries, ref a[i], struct elements, and carrying on after the exception.
// Compared with Mono line for line.
struct Pt { public int X, Y; }

class Program {
    static string Try(Action a) {
        try { a(); return "ok"; }
        catch (IndexOutOfRangeException) { return "IndexOutOfRange"; }
        catch (NullReferenceException) { return "NullReference"; }
        catch (Exception e) { return e.GetType().Name; }
    }
    static void Bump(ref int x) { x++; }

    static string ReadWrite<T>(T[] arr, T value, int[] indexes) {
        string r = "";
        foreach (int i in indexes) {
            r += Try(() => { T x = arr[i]; }) + "/" + Try(() => { arr[i] = value; }) + " ";
        }
        return r;
    }

    static void Main() {
        int[] idx = { -1, 0, 1, 2, 3, 4, int.MaxValue, int.MinValue };   // arrays below have 3 elements
        Console.WriteLine("byte   " + ReadWrite(new byte[3], (byte)1, idx));
        Console.WriteLine("sbyte  " + ReadWrite(new sbyte[3], (sbyte)1, idx));
        Console.WriteLine("short  " + ReadWrite(new short[3], (short)1, idx));
        Console.WriteLine("ushort " + ReadWrite(new ushort[3], (ushort)1, idx));
        Console.WriteLine("int    " + ReadWrite(new int[3], 1, idx));
        Console.WriteLine("uint   " + ReadWrite(new uint[3], 1u, idx));
        Console.WriteLine("long   " + ReadWrite(new long[3], 1L, idx));
        Console.WriteLine("float  " + ReadWrite(new float[3], 1f, idx));
        Console.WriteLine("double " + ReadWrite(new double[3], 1.0, idx));
        Console.WriteLine("bool   " + ReadWrite(new bool[3], true, idx));
        Console.WriteLine("char   " + ReadWrite(new char[3], 'a', idx));
        Console.WriteLine("string " + ReadWrite(new string[3], "s", idx));
        Console.WriteLine("object " + ReadWrite(new object[3], new object(), idx));
        Console.WriteLine("Pt     " + ReadWrite(new Pt[3], new Pt(), idx));

        // an empty array has no valid index; a null array has none either
        Console.WriteLine("empty  " + ReadWrite(new int[0], 1, new int[] { 0, -1 }));
        int[] nul = null; float[] fnul = null; Pt[] pnul = null; object[] onul = null;
        Console.WriteLine("null   " + Try(() => { int x = nul[0]; }) + " " + Try(() => { nul[0] = 1; }) + " " + Try(() => { float x = fnul[1]; }) +
            " " + Try(() => { pnul[0].X = 1; }) + " " + Try(() => { object x = onul[0]; }) + " " + Try(() => { int n = nul.Length; }));

        // ref a[i] (ldelema), and a field of a struct element
        int[] a = new int[3]; Pt[] pts = new Pt[2]; int five = 5; int neg = -1;
        Console.WriteLine("ref    " + Try(() => Bump(ref a[2])) + " " + Try(() => Bump(ref a[three()])) + " " + Try(() => Bump(ref a[neg])) +
            " a[2]=" + a[2] + " " + Try(() => { pts[1].X = 7; }) + " " + Try(() => { pts[five].X = 7; }) + " x=" + pts[1].X);

        // what happened before the fault happened, nothing after it did, and the loop can go on
        int[] data = new int[5]; int done = 0, faults = 0;
        for (int i = 0; i < 12; i++) {
            try { data[i % 7] = i + 1; done++; } catch (IndexOutOfRangeException) { faults++; }
        }
        Console.WriteLine("loop   done=" + done + " faults=" + faults + " data=" + data[0] + "," + data[1] + "," + data[2] + "," + data[3] + "," + data[4]);
        int sum = 0;
        try { for (int i = 0; i <= data.Length; i++) { sum += data[i]; } } catch (IndexOutOfRangeException) { sum = -sum; }
        Console.WriteLine("sum    " + sum);
    }
    static int three() { return 3; }
}
