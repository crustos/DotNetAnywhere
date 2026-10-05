using System;

// An array indexed with a uint, a long or a ulong (and newarr sized by one): the C# compiler puts a conv.u / conv.i before the element opcode, which is
// pointer-wide on a 64-bit target.  The element opcodes take a 32-bit index, so the evaluation stack used to be left misaligned and the program crashed.
// Every kind of element (4, 8, 1, 2-byte numbers, bool, references, a 12-byte struct), load and store, ldelema, compound assignment, an index that
// is an expression; compared with Mono.
class Program {
    struct S { public int A, B, C; }
    class Node { public int V; }

    static uint h;
    static void Mix(uint v) { h = (h ^ v) * 16777619u; }
    static void MixL(long v) { Mix((uint)v); Mix((uint)(v >> 32)); }
    static void Group(string name) { Console.WriteLine(name + " " + h); h = 2166136261u; }
    static void Inc(ref int x) { x += 7; }
    static void Add(ref long x, long by) { x += by; }

    static void Main() {
        h = 2166136261u;
        int[] ia = new int[16]; long[] la = new long[16]; float[] fa = new float[16]; double[] da = new double[16];
        byte[] ba = new byte[16]; short[] sa = new short[16]; bool[] za = new bool[16]; Node[] na = new Node[16];
        S[] ss = new S[16]; object[] oa = new object[16];

        // stores, indexed by a uint: the value on top is 4, 8, 1, 2 bytes, a reference, a 12-byte struct
        for (uint u = 0; u < 16; u++) {
            ia[u] = (int)u * 3; la[u] = (long)u << 33; fa[u] = u * 0.5f; da[u] = u * 0.25; ba[u] = (byte)(u * 7); sa[u] = (short)(u * -300);
            za[u] = (u & 1) == 0; na[u] = new Node(); na[u].V = (int)u; oa[u] = "o" + u;
            S s = new S(); s.A = (int)u; s.B = (int)u * 2; s.C = -(int)u; ss[u] = s;
        }
        for (int i = 0; i < 16; i++) { Mix((uint)ia[i]); MixL(la[i]); Mix((uint)(int)(fa[i] * 100)); MixL((long)(da[i] * 1000)); Mix(ba[i]); Mix((uint)sa[i]); Mix(za[i] ? 1u : 0u); Mix((uint)na[i].V); Mix((uint)ss[i].B + (uint)ss[i].C); Mix((uint)oa[i].ToString().Length); }
        Group("store by uint");

        // loads, indexed by a uint, a long, a ulong
        for (uint u = 0; u < 16; u++) { Mix((uint)ia[u]); MixL(la[u]); Mix((uint)(int)(fa[u] * 100)); MixL((long)(da[u] * 1000)); Mix(ba[u]); Mix((uint)sa[u]); Mix(za[u] ? 1u : 0u); Mix((uint)na[u].V); Mix((uint)ss[u].A); Mix((uint)oa[u].ToString().Length); }
        Group("load by uint");
        for (long l = 0; l < 16; l++) { Mix((uint)ia[l]); MixL(la[l]); Mix((uint)(int)(fa[l] * 100)); MixL((long)(da[l] * 1000)); Mix(ba[l]); Mix((uint)sa[l]); Mix(za[l] ? 1u : 0u); Mix((uint)na[l].V); Mix((uint)ss[l].C); }
        Group("load by long");
        for (ulong g = 0; g < 16; g++) { Mix((uint)ia[g]); MixL(la[g]); Mix((uint)ss[g].B); Mix((uint)na[g].V); }
        Group("load by ulong");

        // stores by a long and a ulong
        for (long l = 0; l < 16; l++) { ia[l] = (int)l * 5; la[l] = l * 1000003; da[l] = l / 8.0; ba[l] = (byte)(l + 1); na[l].V = (int)l * 11; }
        for (ulong g = 0; g < 16; g++) { fa[g] = g * 0.125f; ss[g].A = (int)g * 13; sa[g] = (short)(g * 9); }
        for (int i = 0; i < 16; i++) { Mix((uint)ia[i]); MixL(la[i]); MixL((long)(da[i] * 1000)); Mix(ba[i]); Mix((uint)na[i].V); Mix((uint)(int)(fa[i] * 1000)); Mix((uint)ss[i].A); Mix((uint)sa[i]); }
        Group("store by long and ulong");

        // ldelema (ref to an element), compound assignment, an index that is an expression
        uint k = 3; long m = 5; ulong q = 9;
        Inc(ref ia[k]); Inc(ref ia[m]); Add(ref la[q], 123456789012L); Add(ref la[m + 1], 5);
        ia[k] += 100; la[q] *= 2; ia[m]++; ia[k + 1] = ia[k] - ia[m + 2]; la[q - 1] -= la[k];
        for (int i = 0; i < 16; i++) { Mix((uint)ia[i]); MixL(la[i]); }
        Group("ldelema, compound, expression index");

        // newarr sized by a uint, a long, a ulong
        uint n1 = 5; long n2 = 7; ulong n3 = 9;
        int[] a1 = new int[n1]; long[] a2 = new long[n2]; S[] a3 = new S[n3]; byte[] a4 = new byte[n1 + 2];
        a1[n1 - 1] = 42; a2[n2 - 1] = 1L << 40; a3[n3 - 1].C = 77; a4[a4.Length - 1] = 9;
        Mix((uint)a1.Length); Mix((uint)a2.Length); Mix((uint)a3.Length); Mix((uint)a4.Length);
        Mix((uint)a1[4]); MixL(a2[6]); Mix((uint)a3[8].C); Mix(a4[6]);
        Group("newarr by uint, long, ulong");
    }
}
