using System;

// Indirect access through `ref`: stores and loads of 64-bit/float/double/IntPtr values (stind.i8,
// stind.r4, stind.r8, stind.i, ldind.i had no translation or handler), and 1- and 2-byte values
// (which were read and written as whole 4-byte words, clobbering neighbouring array elements
// whenever the ref pointed into a byte[]/short[]/char[]). Locals and fields are 4-byte slots in
// this runtime, so they must keep working too.
class Holder { public byte B; public sbyte SB; public short S; public ushort US; public char C; }

class Program {
    static void SetL(ref long x, long v) { x = v; }
    static void SetUL(ref ulong x, ulong v) { x = v; }
    static void SetF(ref float x, float v) { x = v; }
    static void SetD(ref double x, double v) { x = v; }
    static void SetP(ref IntPtr x, IntPtr v) { x = v; }
    static IntPtr GetP(ref IntPtr x) { return x; }
    static void SetB(ref byte x, byte v) { x = v; }
    static void SetSB(ref sbyte x, sbyte v) { x = v; }
    static void SetS(ref short x, short v) { x = v; }
    static void SetUS(ref ushort x, ushort v) { x = v; }
    static void SetC(ref char x, char v) { x = v; }
    static int GetB(ref byte x) { return x; }
    static int GetSB(ref sbyte x) { return x; }
    static int GetS(ref short x) { return x; }
    static int GetUS(ref ushort x) { return x; }
    static int GetC(ref char x) { return x; }

    public static int Main() {
        // ---- 64-bit, float, double, IntPtr
        long l = 0; SetL(ref l, -5L); if (l != -5L) return 1;
        ulong ul = 0; SetUL(ref ul, ulong.MaxValue); if (ul != ulong.MaxValue) return 2;
        float f = 0; SetF(ref f, 2.5f); if (f != 2.5f) return 3;
        double d = 0; SetD(ref d, -1.25); if (d != -1.25) return 4;
        IntPtr p = IntPtr.Zero; SetP(ref p, new IntPtr(9)); if (p != new IntPtr(9) || GetP(ref p) != p) return 5;
        long[] la = new long[3]; SetL(ref la[1], 1L << 40); if (la[0] != 0 || la[1] != (1L << 40) || la[2] != 0) return 6;
        double[] da = new double[3]; SetD(ref da[2], 8.5); if (da[1] != 0 || da[2] != 8.5) return 7;
        float[] fa = new float[3]; SetF(ref fa[0], 3.5f); if (fa[0] != 3.5f || fa[1] != 0) return 8;
        IntPtr[] pa = new IntPtr[3]; pa[1] = new IntPtr(5); SetP(ref pa[2], new IntPtr(6));
        if (pa[0] != IntPtr.Zero || pa[1] != new IntPtr(5) || pa[2] != new IntPtr(6) || GetP(ref pa[1]) != new IntPtr(5)) return 9;

        // ---- 1- and 2-byte elements of arrays: exactly the element, never its neighbours
        byte[] ba = new byte[] { 1, 2, 3, 4 };
        SetB(ref ba[0], 9); if (ba[0] != 9 || ba[1] != 2 || ba[2] != 3 || ba[3] != 4) return 10;
        SetB(ref ba[3], 250); if (ba[2] != 3 || ba[3] != 250) return 11;
        if (GetB(ref ba[1]) != 2 || GetB(ref ba[3]) != 250) return 12;
        sbyte[] sa = new sbyte[] { -5, 2, 3, 4 };
        if (GetSB(ref sa[0]) != -5) return 13;
        SetSB(ref sa[1], -100); if (sa[0] != -5 || sa[1] != -100 || sa[2] != 3) return 14;
        short[] sh = new short[] { 1, 2, 3, 4 };
        SetS(ref sh[0], -1); if (sh[0] != -1 || sh[1] != 2 || sh[2] != 3) return 15;
        if (GetS(ref sh[0]) != -1) return 16;
        short[] sh2 = new short[] { -5, 2 }; if (GetS(ref sh2[0]) != -5) return 17;
        ushort[] ua = new ushort[] { 60000, 2, 3 };
        if (GetUS(ref ua[0]) != 60000) return 18;
        SetUS(ref ua[1], 65535); if (ua[0] != 60000 || ua[1] != 65535 || ua[2] != 3) return 19;
        char[] ca = new char[] { (char)1, (char)2, (char)3 };
        SetC(ref ca[0], (char)65); if (ca[0] != 65 || ca[1] != 2 || ca[2] != 3) return 20;
        if (GetC(ref ca[0]) != 65) return 21;

        // ---- locals and fields hold values widened to 32 bits; narrow stores must keep them valid
        byte lb; SetB(out lb); if (lb != 200) return 30;
        sbyte lsb = 5; SetSB(ref lsb, -3); if (lsb != -3) return 31;
        sbyte lsb2 = -1; SetSB(ref lsb2, 1); if (lsb2 != 1) return 32;
        SetSB(ref lsb2, -2); if (lsb2 != -2) return 33;
        short ls = -1; SetS(ref ls, 300); if (ls != 300 || ls + 1 != 301) return 34;
        SetS(ref ls, -300); if (ls != -300 || ls + 1 != -299) return 35;
        ushort lus = 1; SetUS(ref lus, 65535); if (lus != 65535) return 36;
        char lc = 'a'; SetC(ref lc, 'z'); if (lc != 'z') return 37;
        Holder h = new Holder();
        SetB(ref h.B, 250); SetSB(ref h.SB, -7); SetS(ref h.S, -300); SetUS(ref h.US, 65000); SetC(ref h.C, 'q');
        if (h.B != 250 || h.SB != -7 || h.S != -300 || h.US != 65000 || h.C != 'q') return 38;
        if (GetB(ref h.B) != 250 || GetSB(ref h.SB) != -7 || GetS(ref h.S) != -300) return 39;
        return 0;
    }
    static void SetB(out byte x) { x = 200; }
}
