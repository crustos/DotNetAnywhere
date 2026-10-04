using System;
using System.Collections.Generic;

// Structs stored into and read out of arrays, compared with Mono line for line. `arr[i] = new S(...)` compiles to
// ldelema + a constructor call + stobj, and stobj popped the destination address as 4 bytes: wrong on a 64-bit
// target, where a pointer is 8, so any struct stored this way left the evaluation stack out of step (found by
// storing a KeyValuePair<int,string> into an array).
struct IntRef { public int A; public string B; public IntRef(int a, string b) { A = a; B = b; } }
struct LongInt { public long A; public int B; public LongInt(long a, int b) { A = a; B = b; } }
struct RefRef { public string A; public object B; public RefRef(string a, object b) { A = a; B = b; } }
struct Three { public int X; public string S; public int Y; public Three(int x, string s, int y) { X = x; S = s; Y = y; } }
struct Inner { public int V; public string W; }
struct Outer { public Inner In; public long L; public Outer(int v, string w, long l) { In.V = v; In.W = w; L = l; } }
struct Small { public byte B; public short S; public Small(byte b, short s) { B = b; S = s; } }

class Program {
    static void Main() {
        IntRef[] a = new IntRef[3];
        a[0] = new IntRef(1, "a"); a[1] = new IntRef(2, "b");
        int idx = 2; a[idx++] = new IntRef(3, "c");
        Console.WriteLine("int+ref: " + a[0].A + a[0].B + a[1].A + a[1].B + a[2].A + a[2].B + " idx " + idx);

        LongInt[] b = new LongInt[3];
        for (int i = 0; i < 3; i++) b[i] = new LongInt(5000000000L + i, i * 10);
        Console.WriteLine("long+int: " + b[0].A + " " + b[1].B + " " + b[2].A + " " + b[2].B);

        RefRef[] r = new RefRef[2];
        r[0] = new RefRef("x", "y"); r[1] = new RefRef("p", 42);
        Console.WriteLine("ref+ref: " + r[0].A + r[0].B + r[1].A + r[1].B);

        Three[] t = new Three[4];
        for (int i = 0; i < 4; i++) t[i] = new Three(i, "s" + i, i * i);
        Console.WriteLine("int+ref+int: " + t[0].X + t[0].S + t[0].Y + " " + t[3].X + t[3].S + t[3].Y);

        Outer[] o = new Outer[2];
        o[0] = new Outer(7, "w7", 70L); o[1] = new Outer(8, "w8", 80L);
        Console.WriteLine("nested: " + o[0].In.V + o[0].In.W + o[0].L + " " + o[1].In.V + o[1].In.W + o[1].L);

        Small[] s = new Small[3];
        s[0] = new Small(200, -5); s[1] = new Small(1, 30000);
        Console.WriteLine("small: " + s[0].B + " " + s[0].S + " " + s[1].B + " " + s[1].S + " " + s[2].B);

        // copying structs around: element to element, to a local, through a list and a KeyValuePair array
        a[0] = a[2]; IntRef local = a[1]; local.A = 99; a[1] = local;
        Console.WriteLine("copies: " + a[0].A + a[0].B + " " + a[1].A + a[1].B + " " + a[2].A + a[2].B);
        List<IntRef> list = new List<IntRef>();
        for (int i = 0; i < 20; i++) list.Add(new IntRef(i, "n" + i));
        long sum = 0; foreach (IntRef x in list) sum += x.A + x.B.Length;
        Console.WriteLine("list of structs: " + list.Count + " " + sum + " " + list[19].B);

        KeyValuePair<int, string>[] kv = new KeyValuePair<int, string>[3];
        kv[0] = new KeyValuePair<int, string>(1, "a"); kv[2] = new KeyValuePair<int, string>(3, "c");
        Console.WriteLine("KeyValuePair: " + kv[0].Key + kv[0].Value + " " + kv[1].Key + (kv[1].Value == null) + " " + kv[2].Key + kv[2].Value);

        // many stores in a loop: a drifting evaluation stack shows up as garbage or a crash well before the end
        Three[] big = new Three[500];
        for (int i = 0; i < big.Length; i++) big[i] = new Three(i, "v", -i);
        long check = 0; for (int i = 0; i < big.Length; i++) check += big[i].X + big[i].Y + big[i].S.Length;
        Console.WriteLine("500 stores: " + check);
    }
}
