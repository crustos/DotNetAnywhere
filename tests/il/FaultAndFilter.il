// `fault` blocks (run only while an exception unwinds past them, never on normal exit), and
// filters written directly in IL: any nonzero verdict accepts, zero declines, and the filter sees
// only the exception on its stack. C# cannot emit fault blocks. @ASM@ is the core library's name.
// Each Test* returns 1 on success; Main returns the number of the first failing test.

.assembly extern @ASM@ { }
.assembly FaultAndFilter { }

.class public auto ansi beforefieldinit P extends [@ASM@]System.Object {
  .field public static int32 faults
  .field public static int32 handled

  .method static void Throw() cil managed {
    .maxstack 1
    newobj instance void [@ASM@]System.InvalidOperationException::.ctor()
    throw
  }

  // ---- fault: not run on normal completion
  .method static int32 TestFaultNotOnNormalExit() cil managed {
    .maxstack 2
    ldc.i4.0
    stsfld int32 P::faults
    .try {
      nop
      leave DONE
    } fault {
      ldsfld int32 P::faults
      ldc.i4.1
      add
      stsfld int32 P::faults
      endfinally
    }
    DONE:
    ldsfld int32 P::faults
    ldc.i4.0
    ceq
    ret
  }

  // ---- fault: run once, before the catch, while the exception passes through
  .method static int32 TestFaultOnException() cil managed {
    .maxstack 2
    ldc.i4.0
    stsfld int32 P::faults
    ldc.i4.0
    stsfld int32 P::handled
    .try {
      .try {
        call void P::Throw()
        leave INNER_DONE
      } fault {
        // the catch below has not run yet
        ldsfld int32 P::handled
        brtrue BAD
        ldsfld int32 P::faults
        ldc.i4.1
        add
        stsfld int32 P::faults
        BAD: endfinally
      }
      INNER_DONE: leave END
    } catch [@ASM@]System.InvalidOperationException {
      pop
      ldc.i4.1
      stsfld int32 P::handled
      leave END
    }
    END:
    ldsfld int32 P::faults
    ldc.i4.1
    bne.un FAIL
    ldsfld int32 P::handled
    ldc.i4.1
    ceq
    ret
    FAIL: ldc.i4.0
    ret
  }

  // fault blocks in two methods, both unwound through by one exception
  .method static void ThrowThroughFault() cil managed {
    .maxstack 2
    .try {
      call void P::Throw()
      leave DONE
    } fault {
      ldsfld int32 P::faults
      ldc.i4.1
      add
      stsfld int32 P::faults
      endfinally
    }
    DONE: ret
  }
  .method static int32 TestFaultTwoFrames() cil managed {
    .maxstack 2
    ldc.i4.0
    stsfld int32 P::faults
    .try {
      .try {
        call void P::ThrowThroughFault()
        leave INNER
      } fault {
        ldsfld int32 P::faults
        ldc.i4.s 10
        add
        stsfld int32 P::faults
        endfinally
      }
      INNER: leave END
    } catch [@ASM@]System.InvalidOperationException {
      pop
      leave END
    }
    END:
    ldsfld int32 P::faults
    ldc.i4.s 11
    ceq
    ret
  }

  // ---- a filter written in IL: returns the verdict given, any nonzero value accepting
  .method static int32 Filtered(int32 verdict) cil managed {
    .maxstack 2
    .locals init (int32 r)
    .try {
      .try {
        call void P::Throw()
        leave DONE
      } filter {
        pop                     // the exception
        ldarg.0                 // the verdict (a method argument is visible to the filter)
        endfilter
      } {
        pop
        ldc.i4.1
        stloc r
        leave DONE
      }
      DONE: leave END
    } catch [@ASM@]System.Object {
      pop
      ldc.i4.2
      stloc r
      leave END
    }
    END: ldloc r
    ret
  }
  .method static int32 TestFilterVerdicts() cil managed {
    .maxstack 2
    ldc.i4.1
    call int32 P::Filtered(int32)
    ldc.i4.1
    bne.un FAIL           // 1: accepted by the filter's own handler
    ldc.i4.5
    call int32 P::Filtered(int32)
    ldc.i4.1
    bne.un FAIL           // 5 is nonzero: also accepted
    ldc.i4.m1
    call int32 P::Filtered(int32)
    ldc.i4.1
    bne.un FAIL           // -1 is nonzero too
    ldc.i4.0
    call int32 P::Filtered(int32)
    ldc.i4.2
    bne.un FAIL           // 0: declined, the outer catch handles it
    ldc.i4.1
    ret
    FAIL: ldc.i4.0
    ret
  }

  // the filter inspects the exception object it is given
  .method static int32 TestFilterSeesException() cil managed {
    .maxstack 2
    .locals init (int32 r)
    .try {
      .try {
        call void P::Throw()
        leave DONE
      } filter {
        isinst [@ASM@]System.InvalidOperationException
        ldnull
        cgt.un                  // is it an InvalidOperationException?
        endfilter
      } {
        pop
        ldc.i4.1
        stloc r
        leave DONE
      }
      DONE: leave END
    } catch [@ASM@]System.Object {
      pop
      ldc.i4.2
      stloc r
      leave END
    }
    END: ldloc r
    ldc.i4.1
    ceq
    ret
  }

  .method public static int32 Main() cil managed {
    .entrypoint
    .maxstack 2
    call int32 P::TestFaultNotOnNormalExit()
    brfalse F1
    call int32 P::TestFaultOnException()
    brfalse F2
    call int32 P::TestFaultTwoFrames()
    brfalse F3
    call int32 P::TestFilterVerdicts()
    brfalse F4
    call int32 P::TestFilterSeesException()
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
