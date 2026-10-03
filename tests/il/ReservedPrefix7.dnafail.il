// EXPECT-CRASH: reserved op-code 0xf8 is not valid in a method body
// Byte 0xf8 is one of the encodings ECMA-335 reserves for future instruction prefixes
// (prefix7..prefix2, prefixref). It is not an instruction and may not appear in a method body,
// so DNA must refuse the method with a clear message instead of misreading it.
.assembly extern @ASM@ { }
.assembly ReservedPrefix7 { }
.class public auto ansi beforefieldinit P extends [@ASM@]System.Object {
  .method public static int32 Main() cil managed {
    .entrypoint
    .maxstack 1
    .emitbyte 0xf8
    ldc.i4.0
    ret
  }
}
