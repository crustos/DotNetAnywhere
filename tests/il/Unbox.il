// The `unbox` instruction (as opposed to unbox.any): type-check a boxed value and yield a pointer
// to the value inside the box, which can then be read or written in place.
// @ASM@ is the core library's name. Each Test* returns 1 on success; Main returns the number of
// the first failing test.
.assembly extern @ASM@ { }
.assembly Unbox { }
.class public sequential ansi sealed beforefieldinit Pt extends [@ASM@]System.ValueType {
  .field public int32 x
  .field public int64 y
}
.class public auto ansi beforefieldinit P extends [@ASM@]System.Object {

  .method static int32 TestRead() cil managed {
    .maxstack 2
    ldc.i4 1234
    box [@ASM@]System.Int32
    unbox [@ASM@]System.Int32
    ldind.i4
    ldc.i4 1234
    ceq
    ret
  }
  // writing through the pointer changes the boxed value itself
  .method static int32 TestWriteThrough() cil managed {
    .maxstack 3
    .locals init (object o)
    ldc.i4 1
    box [@ASM@]System.Int32
    stloc o
    ldloc o
    unbox [@ASM@]System.Int32
    ldc.i4 99
    stind.i4
    ldloc o
    unbox.any [@ASM@]System.Int32
    ldc.i4 99
    ceq
    ret
  }
  .method static int32 TestStructField() cil managed {
    .maxstack 3
    .locals init (valuetype Pt p, object o)
    ldloca p
    ldc.i4 -7
    stfld int32 Pt::x
    ldloca p
    ldc.i8 0x123456789
    stfld int64 Pt::y
    ldloc p
    box Pt
    stloc o
    ldloc o
    unbox Pt
    ldfld int32 Pt::x
    ldc.i4 -7
    bne.un FAIL
    ldloc o
    unbox Pt
    ldfld int64 Pt::y
    ldc.i8 0x123456789
    ceq
    ret
    FAIL: ldc.i4.0
    ret
  }
  // the wrong type is an InvalidCastException; null is a NullReferenceException.
  // result: 0 = no exception, 1 = InvalidCast, 2 = NullReference, 3 = other
  .method static int32 Try(object o) cil managed {
    .maxstack 2
    .locals init (int32 r)
    .try {
      ldarg.0
      unbox [@ASM@]System.Int32
      pop
      ldc.i4.0
      stloc r
      leave DONE
    } catch [@ASM@]System.InvalidCastException {
      pop
      ldc.i4.1
      stloc r
      leave DONE
    } catch [@ASM@]System.NullReferenceException {
      pop
      ldc.i4.2
      stloc r
      leave DONE
    }
    DONE: ldloc r
    ret
  }
  .method static int32 TestChecks() cil managed {
    .maxstack 3
    ldc.i4.5
    box [@ASM@]System.Int32
    call int32 P::Try(object)
    brtrue FAIL                  // int as int: fine
    ldstr "text"
    call int32 P::Try(object)
    ldc.i4.1
    bne.un FAIL                  // string as int
    ldc.i4.5
    conv.i8
    box [@ASM@]System.Int64
    call int32 P::Try(object)
    ldc.i4.1
    bne.un FAIL                  // long as int
    ldnull
    call int32 P::Try(object)
    ldc.i4.2
    bne.un FAIL                  // null
    ldc.i4.1
    ret
    FAIL: ldc.i4.0
    ret
  }

  .method public static int32 Main() cil managed {
    .entrypoint
    .maxstack 2
    call int32 P::TestRead()
    brfalse F1
    call int32 P::TestWriteThrough()
    brfalse F2
    call int32 P::TestStructField()
    brfalse F3
    call int32 P::TestChecks()
    brfalse F4
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
  }
}
