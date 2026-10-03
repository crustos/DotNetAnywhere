using System;

// Converting enum values to text, for every underlying type that fits the runtime's 32-bit enum
// handling: named members, undefined values, negative values, and [Flags] combinations. Only
// enums whose underlying type was int used to work (the member constants were read as int32
// only). The output is compared with Mono's, line for line.
enum EInt : int { A = 1, B = 2, C = 300, Neg = -5 }
enum EUInt : uint { A = 1, B = 4000000000u }
enum EByte : byte { A = 1, B = 200, C = 255 }
enum ESByte : sbyte { A = 1, B = -128, C = 127, D = -1 }
enum EShort : short { A = 1, B = -32768, C = 32767 }
enum EUShort : ushort { A = 1, B = 65535 }
[Flags] enum FByte : byte { None = 0, R = 1, W = 2, X = 4, All = 7 }
[Flags] enum FInt : int { P = 1, Q = 2, S = 8 }
[Flags] enum FUShort : ushort { Lo = 1, Hi = 256 }

class Program {
    static void Main() {
        Console.WriteLine("int:    " + EInt.A + " " + EInt.B + " " + EInt.C + " " + EInt.Neg + " | " + (EInt)7 + " " + (EInt)(-5) + " " + (EInt)0);
        Console.WriteLine("uint:   " + EUInt.A + " " + EUInt.B + " | " + (EUInt)7u);
        Console.WriteLine("byte:   " + EByte.A + " " + EByte.B + " " + EByte.C + " | " + (EByte)7 + " " + (EByte)0);
        Console.WriteLine("sbyte:  " + ESByte.A + " " + ESByte.B + " " + ESByte.C + " " + ESByte.D + " | " + (ESByte)7 + " " + (ESByte)(-2));
        Console.WriteLine("short:  " + EShort.A + " " + EShort.B + " " + EShort.C + " | " + (EShort)7 + " " + (EShort)(-7));
        Console.WriteLine("ushort: " + EUShort.A + " " + EUShort.B + " | " + (EUShort)7);
        Console.WriteLine("flags byte:   " + FByte.R + " " + FByte.None + " " + FByte.All + " | " + (FByte.R | FByte.W) + " | " + (FByte)9);
        Console.WriteLine("flags int:    " + FInt.P + " " + (FInt.P | FInt.S) + " " + (FInt.P | FInt.Q | FInt.S) + " | " + (FInt)0 + " " + (FInt)16);
        Console.WriteLine("flags ushort: " + FUShort.Lo + " " + FUShort.Hi + " " + (FUShort.Lo | FUShort.Hi));

        // the value survives a trip through object and back, and names look up from the type
        object boxed = EByte.B;
        Console.WriteLine("boxed:  " + boxed + " " + (EByte)boxed + " " + ((int)(EByte)boxed));
        Console.WriteLine("names:  " + string.Join(",", Enum.GetNames(typeof(EByte))));
        Console.WriteLine("names:  " + string.Join(",", Enum.GetNames(typeof(ESByte))));
        Console.WriteLine("name:   " + Enum.GetName(typeof(EShort), EShort.C) + " " + Enum.GetName(typeof(EUInt), EUInt.B));
        // comparing, switching and hashing do not depend on the text
        EByte b = EByte.C;
        switch (b) { case EByte.A: Console.WriteLine("sw: A"); break; case EByte.C: Console.WriteLine("sw: C"); break; default: Console.WriteLine("sw: ?"); break; }
        Console.WriteLine("cmp:    " + (EByte.A < EByte.B) + " " + (ESByte.B < ESByte.A) + " " + (EUInt.B > EUInt.A) + " " + (EShort.B < EShort.A));
    }
}
