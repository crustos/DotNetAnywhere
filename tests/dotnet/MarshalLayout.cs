using System;
using System.Runtime.InteropServices;

// Layouts where DNA's in-memory field layout (every small field in a 4-byte slot, no
// padding) differs from the reference runtime's Sequential layout. Expected values were
// checked against Mono, not just derived by hand.

public enum Kind { A, B, C }
public enum Small : byte { X = 1, Y = 200 }

public struct Natural { public byte Tag; public int Id; }                          // 8
[StructLayout(LayoutKind.Sequential, Pack = 1)]
public struct Packed1 { public byte Tag; public int Id; }                          // 5
[StructLayout(LayoutKind.Sequential, Pack = 2)]
public struct Packed2 { public byte A; public int B; public byte C; }              // 8
public struct Head { public Kind K; public short Len; public byte Flags; public byte Pad; }  // 8
public struct Msg { public Head H; public long Seq; public double X; }             // 24
public struct Signed { public sbyte B; public short S; public int I; public long L; }       // 16
public struct Unsigned { public byte B; public ushort S; public uint I; public ulong L; }   // 16
public struct WithEnum { public Small S; public byte After; }                      // 2
public struct WithStatic { public static int Z = 5; public int V; }                // 4
public struct Floats { public float F; public double D; }                          // 16
public struct Pair { public short A; public short B; }                             // 4

public class Program {
    static int Fail(int code) { return code; }

    public static int Main() {
        // --- sizes
        if (Marshal.SizeOf<Natural>() != 8) return 1;
        if (Marshal.SizeOf<Packed1>() != 5) return 2;
        if (Marshal.SizeOf<Packed2>() != 8) return 3;
        if (Marshal.SizeOf<Head>() != 8) return 4;
        if (Marshal.SizeOf<Msg>() != 24) return 5;
        if (Marshal.SizeOf<Signed>() != 16) return 6;
        if (Marshal.SizeOf<WithEnum>() != 2) return 7;
        if (Marshal.SizeOf<WithStatic>() != 4) return 8;
        if (Marshal.SizeOf<Floats>() != 16) return 9;

        // --- exact bytes: natural alignment pads, Pack = 1 does not
        Natural n = new Natural(); n.Tag = 0x11; n.Id = 0x04030201;
        byte[] b = MemoryMarshal.AsBytes(MemoryMarshal.CreateSpan(ref n, 1)).ToArray();
        if (b.Length != 8 || b[0] != 0x11 || b[4] != 1 || b[5] != 2 || b[6] != 3 || b[7] != 4) return 10;

        Packed1 p1 = new Packed1(); p1.Tag = 0x11; p1.Id = 0x04030201;
        b = MemoryMarshal.AsBytes(MemoryMarshal.CreateSpan(ref p1, 1)).ToArray();
        if (b.Length != 5 || b[0] != 0x11 || b[1] != 1 || b[2] != 2 || b[3] != 3 || b[4] != 4) return 11;
        Packed1 p1r = MemoryMarshal.Read<Packed1>(b);
        if (p1r.Tag != 0x11 || p1r.Id != 0x04030201) return 12;

        Packed2 p2 = new Packed2(); p2.A = 7; p2.B = 0x0A0B0C0D; p2.C = 9;
        b = MemoryMarshal.AsBytes(MemoryMarshal.CreateSpan(ref p2, 1)).ToArray();
        if (b.Length != 8 || b[0] != 7 || b[1] != 0 || b[2] != 0x0D || b[5] != 0x0A || b[6] != 9) return 13;
        Packed2 p2r = MemoryMarshal.Read<Packed2>(b);
        if (p2r.A != 7 || p2r.B != 0x0A0B0C0D || p2r.C != 9) return 14;

        // --- small fields packed tightly; enum field; nested struct
        Head h = new Head(); h.K = Kind.C; h.Len = 300; h.Flags = 0xAB; h.Pad = 0xCD;
        b = MemoryMarshal.AsBytes(MemoryMarshal.CreateSpan(ref h, 1)).ToArray();
        if (b.Length != 8) return 15;
        if (b[0] != 2 || b[1] != 0 || b[2] != 0 || b[3] != 0) return 16;
        if (b[4] != 0x2C || b[5] != 0x01 || b[6] != 0xAB || b[7] != 0xCD) return 17;

        Msg m = new Msg(); m.H = h; m.Seq = 1234567890123L; m.X = 0.5;
        b = MemoryMarshal.AsBytes(MemoryMarshal.CreateSpan(ref m, 1)).ToArray();
        Msg mr = MemoryMarshal.Read<Msg>(b);
        if (b.Length != 24) return 18;
        if (mr.Seq != 1234567890123L || mr.X != 0.5) return 19;
        if (mr.H.K != Kind.C || mr.H.Len != 300 || mr.H.Flags != 0xAB || mr.H.Pad != 0xCD) return 20;

        // --- sign / zero extension survives the round trip
        Signed s = new Signed(); s.B = -1; s.S = -2; s.I = -3; s.L = -4;
        b = MemoryMarshal.AsBytes(MemoryMarshal.CreateSpan(ref s, 1)).ToArray();
        if (b[0] != 0xFF || b[2] != 0xFE || b[3] != 0xFF) return 21;
        Signed sr = MemoryMarshal.Read<Signed>(b);
        if (sr.B != -1 || sr.S != -2 || sr.I != -3 || sr.L != -4) return 22;

        Unsigned u = new Unsigned(); u.B = 255; u.S = 65535; u.I = 0xFFFFFFFF; u.L = ulong.MaxValue;
        b = MemoryMarshal.AsBytes(MemoryMarshal.CreateSpan(ref u, 1)).ToArray();
        Unsigned ur = MemoryMarshal.Read<Unsigned>(b);
        if (ur.B != 255 || ur.S != 65535 || ur.I != 0xFFFFFFFF || ur.L != ulong.MaxValue) return 23;

        WithEnum we = new WithEnum(); we.S = Small.Y; we.After = 0x7F;
        b = MemoryMarshal.AsBytes(MemoryMarshal.CreateSpan(ref we, 1)).ToArray();
        if (b.Length != 2 || b[0] != 200 || b[1] != 0x7F) return 24;
        WithEnum wer = MemoryMarshal.Read<WithEnum>(b);
        if (wer.S != Small.Y || wer.After != 0x7F) return 25;

        // --- floating point bit patterns
        Floats f = new Floats(); f.F = 1.5f; f.D = -2.0;
        b = MemoryMarshal.AsBytes(MemoryMarshal.CreateSpan(ref f, 1)).ToArray();
        if (b[0] != 0 || b[1] != 0 || b[2] != 0xC0 || b[3] != 0x3F) return 26;     // 1.5f = 0x3FC00000
        if (b[15] != 0xC0 || b[14] != 0x00) return 27;                              // -2.0 = 0xC000000000000000

        // --- Write into a buffer at an offset through a span; Read it back through a slice
        Pair pr = new Pair(); pr.A = 1; pr.B = -1;
        byte[] buf = new byte[10];
        MemoryMarshal.Write(new Span<byte>(buf, 3, 4), ref pr);
        if (buf[2] != 0 || buf[3] != 1 || buf[4] != 0 || buf[5] != 0xFF || buf[6] != 0xFF || buf[7] != 0) return 28;
        Pair prr = MemoryMarshal.Read<Pair>(new ReadOnlySpan<byte>(buf, 3, 4));
        if (prr.A != 1 || prr.B != -1) return 29;

        // --- a span over several elements, and Cast
        Pair[] arr = new Pair[3];
        for (int i = 0; i < 3; i++) { arr[i].A = (short)(i + 1); arr[i].B = (short)(-(i + 1)); }
        b = MemoryMarshal.AsBytes(new Span<Pair>(arr)).ToArray();
        if (b.Length != 12 || b[4] != 2 || b[6] != 0xFE || b[8] != 3) return 30;
        Span<int> ints = MemoryMarshal.Cast<byte, int>(new Span<byte>(new byte[] { 1, 2, 3, 4, 5, 0, 0, 0 }));
        if (ints.Length != 2 || ints[0] != 0x04030201 || ints[1] != 5) return 31;

        // --- errors
        bool threw = false;
        try { MemoryMarshal.Read<Natural>(new byte[7]); } catch (ArgumentOutOfRangeException) { threw = true; }
        if (!threw) return 33;
        threw = false;
        try { MemoryMarshal.Write(new Span<byte>(new byte[3]), ref pr); } catch (ArgumentOutOfRangeException) { threw = true; }
        if (!threw) return 34;
        return 0;
    }
}
