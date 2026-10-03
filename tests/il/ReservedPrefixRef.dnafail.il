// EXPECT-CRASH: reserved op-code 0xff is not valid in a method body
// Byte 0xff is one of the encodings ECMA-335 reserves for future instruction prefixes
// (prefix7..prefix2, prefixref). It is not an instruction and may not appear in a method body,
// so DNA must refuse the method with a clear message instead of misreading it.
.assembly extern @ASM@ { }
.assembly ReservedPrefixRef { }
.class public auto ansi beforefieldinit P extends [@ASM@]System.Object {
  .method public static int32 Main() cil managed {
    .entrypoint
    .maxstack 1
    .emitbyte 0xff
    ldc.i4.0
    ret
  }
}
