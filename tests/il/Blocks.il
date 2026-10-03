// cpblk (block copy, overlapping ranges allowed) and initblk (block fill). @ASM@ is the core
// library's name. Each Test* returns 1 on success; Main returns the first failing test's number.
// Overlapping ranges are deliberately not tested: ECMA-335 leaves cpblk's result there unspecified
// (Mono copies forwards; DNA uses memmove), so there is no behaviour to compare.
.assembly extern @ASM@ { }
.assembly Blocks { }
.class public auto ansi beforefieldinit P extends [@ASM@]System.Object {

  // fill 4 bytes in the middle of a byte[8] with 0xAB; the rest must be untouched
  .method static int32 TestInitblk() cil managed {
    .maxstack 4
    .locals init (uint8[] a, int32 i)
    ldc.i4.8
    newarr [@ASM@]System.Byte
    stloc a
    ldloc a
    ldc.i4.2
    ldelema [@ASM@]System.Byte
    ldc.i4 0xAB
    ldc.i4.4
    initblk
    ldloc a
    ldc.i4.0
    ldelem.u1
    brtrue FAIL
    ldloc a
    ldc.i4.1
    ldelem.u1
    brtrue FAIL
    ldloc a
    ldc.i4.2
    ldelem.u1
    ldc.i4 0xAB
    bne.un FAIL
    ldloc a
    ldc.i4.5
    ldelem.u1
    ldc.i4 0xAB
    bne.un FAIL
    ldloc a
    ldc.i4.6
    ldelem.u1
    brtrue FAIL
    ldloc a
    ldc.i4.7
    ldelem.u1
    brtrue FAIL
    ldc.i4.1
    ret
    FAIL: ldc.i4.0
    ret
  }
  // only the low byte of the value is used
  .method static int32 TestInitblkLowByte() cil managed {
    .maxstack 4
    .locals init (uint8[] a)
    ldc.i4.4
    newarr [@ASM@]System.Byte
    stloc a
    ldloc a
    ldc.i4.0
    ldelema [@ASM@]System.Byte
    ldc.i4 0x1234
    ldc.i4.4
    initblk
    ldloc a
    ldc.i4.3
    ldelem.u1
    ldc.i4 0x34
    ceq
    ret
  }
  .method static int32 TestCpblk() cil managed {
    .maxstack 4
    .locals init (uint8[] src, uint8[] dst, int32 i)
    ldc.i4.8
    newarr [@ASM@]System.Byte
    stloc src
    ldc.i4.8
    newarr [@ASM@]System.Byte
    stloc dst
    ldc.i4.0
    stloc i
    LOOP:
    ldloc src
    ldloc i
    ldloc i
    ldc.i4.1
    add
    stelem.i1
    ldloc i
    ldc.i4.1
    add
    stloc i
    ldloc i
    ldc.i4.8
    blt LOOP
    // copy src[2..5] to dst[1..4]
    ldloc dst
    ldc.i4.1
    ldelema [@ASM@]System.Byte
    ldloc src
    ldc.i4.2
    ldelema [@ASM@]System.Byte
    ldc.i4.4
    cpblk
    ldloc dst
    ldc.i4.0
    ldelem.u1
    brtrue FAIL
    ldloc dst
    ldc.i4.1
    ldelem.u1
    ldc.i4.3
    bne.un FAIL
    ldloc dst
    ldc.i4.4
    ldelem.u1
    ldc.i4.6
    bne.un FAIL
    ldloc dst
    ldc.i4.5
    ldelem.u1
    brtrue FAIL
    ldc.i4.1
    ret
    FAIL: ldc.i4.0
    ret
  }
  // ints copied as a block land intact (a sanity check on widths, not just bytes)
  .method static int32 TestCpblkInts() cil managed {
    .maxstack 4
    .locals init (int32[] a, int32[] b)
    ldc.i4.3
    newarr [@ASM@]System.Int32
    stloc a
    ldc.i4.3
    newarr [@ASM@]System.Int32
    stloc b
    ldloc a
    ldc.i4.0
    ldc.i4 0x11223344
    stelem.i4
    ldloc a
    ldc.i4.1
    ldc.i4 -5
    stelem.i4
    ldloc a
    ldc.i4.2
    ldc.i4 0x7fffffff
    stelem.i4
    ldloc b
    ldc.i4.0
    ldelema [@ASM@]System.Int32
    ldloc a
    ldc.i4.0
    ldelema [@ASM@]System.Int32
    ldc.i4.s 12
    cpblk
    ldloc b
    ldc.i4.0
    ldelem.i4
    ldc.i4 0x11223344
    bne.un FAIL
    ldloc b
    ldc.i4.1
    ldelem.i4
    ldc.i4 -5
    bne.un FAIL
    ldloc b
    ldc.i4.2
    ldelem.i4
    ldc.i4 0x7fffffff
    ceq
    ret
    FAIL: ldc.i4.0
    ret
  }
  // a zero-length block does nothing, even through null
  .method static int32 TestZeroLength() cil managed {
    .maxstack 4
    ldnull
    ldnull
    ldc.i4.0
    cpblk
    ldnull
    ldc.i4.0
    ldc.i4.0
    initblk
    ldc.i4.1
    ret
  }

  .method public static int32 Main() cil managed {
    .entrypoint
    .maxstack 2
    call int32 P::TestInitblk()
    brfalse F1
    call int32 P::TestInitblkLowByte()
    brfalse F2
    call int32 P::TestCpblk()
    brfalse F3
    call int32 P::TestCpblkInts()
    brfalse F4
    call int32 P::TestZeroLength()
    brfalse F5
    ldc.i4.0
    ret
    F1: ldc.i4.1
    ret
    F2: ldc.i4.2
    ret
    F3: ldc.i4.3
    ret
    F4: ldc.i4.4
    ret
    F5: ldc.i4.5
    ret
  }
}
