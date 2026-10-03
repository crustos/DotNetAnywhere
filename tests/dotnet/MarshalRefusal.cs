using System;
using System.Runtime.InteropServices;

// DNA-specific, NOT parity with the reference runtime: there Marshal.SizeOf marshals a
// string/reference field as a pointer and succeeds. This Marshal only handles unmanaged
// types (primitives, enums, structs of those) and refuses the rest, as Crust's subset does.
public struct HasString { public string S; }
public struct HasArray { public int[] A; }
public struct Nested { public int X; public HasString Inner; }
public class AClass { public int X; }
public struct Ok { public int X; public short Y; }

public class Program {
    static bool Refused(Type t) {
        try { Marshal.SizeOf(t); return false; } catch (ArgumentException) { return true; }
    }
    public static int Main() {
        if (!Refused(typeof(HasString))) return 1;
        if (!Refused(typeof(HasArray))) return 2;
        if (!Refused(typeof(Nested))) return 3;
        if (!Refused(typeof(AClass))) return 4;
        if (!Refused(typeof(string))) return 5;
        if (Refused(typeof(Ok))) return 6;
        if (Marshal.SizeOf(typeof(Ok)) != 8) return 7;
        bool threw = false;
        try { Marshal.SizeOf((Type)null); } catch (ArgumentNullException) { threw = true; }
        if (!threw) return 8;
        // serialising one is refused too
        threw = false;
        HasString hs = new HasString();
        try { MemoryMarshal.AsBytes(MemoryMarshal.CreateSpan(ref hs, 1)); } catch (ArgumentException) { threw = true; }
        if (!threw) return 9;
        // CreateSpan is a one-element snapshot: any other length is refused, not faked
        threw = false;
        Ok ok = new Ok();
        try { MemoryMarshal.CreateSpan(ref ok, 2); } catch (NotSupportedException) { threw = true; }
        if (!threw) return 10;
        return 0;
    }
}
