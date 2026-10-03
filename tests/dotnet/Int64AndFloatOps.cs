using System;

// Operations on 64-bit integers and floating point that had no interpreter handler, or the wrong
// one: unsigned 64-bit branches, overflow-checked 64-bit arithmetic, float/double remainder,
// and 64-bit/IntPtr conversion to text. Expectations are checked against Mono.
class Program {
    static bool Ovf(Action a) { try { a(); return false; } catch (OverflowException) { return true; } }

    public static int Main() {
        // ---- unsigned 64-bit comparisons (bge.un / bgt.un / ble.un / blt.un on int64)
        ulong a = 5, b = 3, hi = 0x8000000000000000UL, one = 1;
        if (!(a >= b) || (b >= a) || !(a >= a)) return 1;
        if (!(a > b) || (b > a) || (a > a)) return 2;
        if (!(b <= a) || (a <= b) || !(a <= a)) return 3;
        if (!(b < a) || (a < b) || (a < a)) return 4;
        if (!(hi > one) || !(hi >= one) || !(one < hi) || !(one <= hi)) return 5;   // high bit set
        if (!(ulong.MaxValue > hi) || (hi > ulong.MaxValue)) return 6;
        long neg = -1;
        if (!(neg < 0) || !(neg < 1L) || (neg > 0)) return 7;                        // signed still signed

        // ---- overflow-checked 64-bit arithmetic
        long lmax = long.MaxValue, lmin = long.MinValue, l1 = 1, lm1 = -1, l2 = 2;
        if (!Ovf(delegate { long r = checked(lmax + l1); })) return 10;
        if (!Ovf(delegate { long r = checked(lmin + lm1); })) return 11;
        if (!Ovf(delegate { long r = checked(lmin - l1); })) return 12;
        if (!Ovf(delegate { long r = checked(lmax - lm1); })) return 13;
        if (!Ovf(delegate { long r = checked(lmax * l2); })) return 14;
        if (!Ovf(delegate { long r = checked(lmin * lm1); })) return 15;
        if (!Ovf(delegate { long r = checked(lm1 * lmin); })) return 16;
        if (checked(lmax - l1 + l1) != lmax || checked(lmin + l1 - l1) != lmin) return 17;
        long t3 = 3000000000L; if (checked(t3 * t3) != 9000000000000000000L) return 18;
        long l0 = 0; if (checked(lmax * l1) != lmax || checked(lmin * l1) != lmin || checked(lmax * l0) != 0) return 19;

        ulong umax = ulong.MaxValue, u1 = 1, u2 = 2, u0 = 0;
        if (!Ovf(delegate { ulong r = checked(umax + u1); })) return 20;
        if (!Ovf(delegate { ulong r = checked(u0 - u1); })) return 21;
        if (!Ovf(delegate { ulong r = checked(umax * u2); })) return 22;
        if (checked(umax - u1 + u1) != umax || checked(umax * u1) != umax || checked(umax * u0) != 0) return 23;
        ulong p32 = 4294967296UL, p32m = 4294967295UL; if (checked(p32 * p32m) != 18446744069414584320UL) return 24;
        if (!Ovf(delegate { ulong r = checked(p32 * p32); })) return 25;

        // ---- float / double remainder
        double d1 = 7.5, d2 = 2.0, dn = -7.5;
        if ((d1 % d2) != 1.5 || (dn % d2) != -1.5 || (d1 % -d2) != 1.5) return 30;
        if (!double.IsNaN(d1 % 0.0) || (d1 % double.PositiveInfinity) != d1) return 31;
        float f1 = 7.5f, f2 = 2f, fn = -7.5f;
        if ((f1 % f2) != 1.5f || (fn % f2) != -1.5f || (f1 % -f2) != 1.5f) return 32;
        if (!float.IsNaN(f1 % 0f)) return 33;
        double acc = 10.25; acc %= 3.0;
        if (acc != 1.25) return 34;

        // ---- 64-bit values to text (needed unsigned 64-bit branches)
        long big = 123456789012L;
        if (("n=" + big) != "n=123456789012") return 40;
        if (long.MinValue.ToString() != "-9223372036854775808") return 41;
        if (long.MaxValue.ToString() != "9223372036854775807") return 42;
        if (ulong.MaxValue.ToString() != "18446744073709551615") return 43;
        if (hi.ToString() != "9223372036854775808") return 44;
        if ((0L).ToString() != "0" || (-1L).ToString() != "-1") return 45;
        return 0;
    }
}
