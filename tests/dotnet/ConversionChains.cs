using System;

// Conversions whose result depends on how the source is interpreted. ECMA-335 decides that by the
// opcode, not by the static type of the variable: a reinterpreting cast such as (int)uintVar emits
// nothing, so (long)(int)uintVar is a bare conv.i8 on a uint local and must sign-extend; only
// conv.u8, conv.r.un and the .un forms of conv.ovf.* treat an integer as unsigned. Also covers
// double -> long, which used to pop 4 bytes of an 8-byte double and corrupt the stack beneath it.
class Program {
    static string Q(long v) { return v == 2 ? "two" : "other"; }
    static long Id(long v) { return v; }
    static string Cat(string a, string b) { return a + b; }

    public static int Main() {
        uint u = 4294967295u; int i = -1; ulong ul = ulong.MaxValue; long l = -1;
        ushort us = 60000; byte bt = 200;

        // reinterpret, then widen: sign-extends
        if ((long)(int)u != -1L) return 1;
        if ((double)(int)u != -1.0) return 2;
        uint u3 = 3000000000u; if ((float)(int)u3 >= 0) return 3;
        if ((double)(long)ul != -1.0) return 4;
        if ((ulong)(int)u != ulong.MaxValue) return 5;
        if ((long)(short)us != -5536L) return 6;
        if ((long)(sbyte)bt != -56L) return 7;

        // genuinely unsigned conversions stay unsigned
        if ((long)(uint)i != 4294967295L) return 10;
        if ((double)u != 4294967295.0) return 11;
        if ((double)ul < 1.8e19) return 12;
        if ((float)ul < 1.8e19f) return 13;
        long widened = u; if (widened != 4294967295L) return 14;
        if ((ulong)u != 4294967295UL) return 15;
        if ((long)us != 60000L || (long)bt != 200L) return 16;
        if (checked((long)u) != 4294967295L) return 17;                 // conv.ovf.i8.un

        // double -> long/ulong with other values on the evaluation stack beneath
        double v = 2.9;
        if (Cat("d=", Q((long)v)) != "d=two") return 20;
        string s = "d=" + Q((long)v); if (s != "d=two") return 21;
        long a = 1; long sum = a + Id((long)v); if (sum != 3) return 22;
        if (Id((long)127.9 + (long)v * 1000) != 2127) return 23;
        double w = -128.9; if ((long)w != -128L || (long)(w * 3) != -386L) return 24;
        double big = 2147483648.5; if ((long)big != 2147483648L) return 25;
        if ((ulong)v != 2UL || (ulong)(v * 1e18) != 2900000000000000000UL) return 26;
        float fv = -2.9f; if ((long)fv != -2L || Id((long)fv) != -2L) return 27;

        // int <-> float/double
        int n = -7; if ((double)n != -7.0 || (float)n != -7f) return 30;
        long big64 = -123456789012L; if ((double)big64 != -123456789012.0) return 31;
        if ((int)7.9 != 7 || (int)-7.9 != -7) return 32;
        return 0;
    }
}
