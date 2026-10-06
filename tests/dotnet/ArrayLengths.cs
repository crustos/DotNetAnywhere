// Array lengths that are not valid: newarr of a negative length is an OverflowException, Array.CreateInstance and Array.Resize with one are an
// ArgumentOutOfRangeException. (DNA used to write past the end of a too-small allocation.) Run the methods that make the arrays in the loop form
// that the wasm JIT compiles, and the same in a method that is interpreted. Output must equal Mono's.
using System;

class ArrayLengths {
    static int Make(int n) { int[] a = new int[n]; return a.Length; }
    static long MakeLoop(int n) { long t = 0; for (int i = -2; i < n; i++) { int[] a = new int[i + 2]; t += a.Length; } return t; }
    static int MakeFloats(int n) { float[] f = new float[n]; double[] d = new double[n]; return f.Length + d.Length; }
    static int MakeStructs(int n) { Pt[] p = new Pt[n]; return p.Length; }
    struct Pt { public int X, Y, Z; }
    static string Try(Func<int> f) {
        try { return f().ToString(); }
        catch (OverflowException) { return "Overflow"; }
        catch (ArgumentOutOfRangeException) { return "ArgumentOutOfRange"; }
    }
    static void Main() {
        int[] ns = { 0, 1, 5, -1, -2, int.MinValue, -1000000 };
        foreach (int n in ns) Console.WriteLine("make " + n + " " + Try(() => Make(n)) + " " + Try(() => MakeFloats(n)) + " " + Try(() => MakeStructs(n)));
        Console.WriteLine("loop " + Try(() => (int)MakeLoop(5)) + " " + Try(() => (int)MakeLoop(0)));
        Console.WriteLine("create " + Try(() => Array.CreateInstance(typeof(int), 3).Length) + " " + Try(() => Array.CreateInstance(typeof(int), -1).Length) + " " + Try(() => Array.CreateInstance(typeof(string), 0).Length));
        int[] a = { 1, 2, 3 };
        Array.Resize(ref a, 5); Console.WriteLine("resize " + a.Length + " " + a[2] + " " + a[4]);
        Array.Resize(ref a, 2); Console.WriteLine("shrink " + a.Length + " " + a[1]);
        Console.WriteLine("resize-neg " + Try(() => { int[] b = { 1 }; Array.Resize(ref b, -1); return b.Length; }));
        int[] nul = null; Array.Resize(ref nul, 4); Console.WriteLine("resize-null " + nul.Length);
        Console.WriteLine("after " + Try(() => Make(7)));
    }
}
