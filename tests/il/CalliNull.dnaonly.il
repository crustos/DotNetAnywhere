// DNA only (the reference runtime aborts on this, so there is nothing to compare with):
// calli through a null function pointer must throw NullReferenceException, not crash.
// Exits 0 on success.
.assembly extern @ASM@ { }
.assembly CalliNull { }
.class public auto ansi beforefieldinit P extends [@ASM@]System.Object {
  .method public static int32 Main() cil managed {
    .entrypoint
    .maxstack 3
    .locals init (int32 r)
    .try {
      ldc.i4 1
      ldc.i4 2
      ldc.i4.0
      conv.i
      calli default int32 (int32, int32)
      pop
      ldc.i4.1
      stloc r
      leave DONE
    } catch [@ASM@]System.NullReferenceException {
      pop
      ldc.i4.0
      stloc r
      leave DONE
    }
    DONE: ldloc r
    ret
  }
}
