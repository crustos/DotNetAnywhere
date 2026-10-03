// Opcodes that C# compilers do not (or rarely) emit, written in IL. @ASM@ is the name of the
// core library: `mscorlib` for the reference runtime, `corlib` for DNA. Each Test* method
// returns 1 on success; Main returns the number of the first failing test, or 0.

.assembly extern @ASM@ { }
.assembly ExtendedOpcodes { }

.class public sequential ansi sealed beforefieldinit Pair extends [@ASM@]System.ValueType {
  .field public int32 a
  .field public int64 b
}
.class public sequential ansi sealed beforefieldinit Small extends [@ASM@]System.ValueType {
  .field public int8 x
  .field public int16 y
}

.class public auto ansi beforefieldinit P extends [@ASM@]System.Object {
  .field public static int32 gstat

  // ---- break, and the prefixes that have no effect here
  .method static int32 TestBreak() cil managed {
    .maxstack 2
    ldc.i4.5
    break
    ldc.i4.5
    ceq
    ret
  }
  .method static int32 TestVolatile() cil managed {
    .maxstack 2
    ldc.i4 41
    volatile.
    stsfld int32 P::gstat
    volatile.
    ldsfld int32 P::gstat
    ldc.i4 41
    ceq
    ret
  }
  .method static int32 TestUnaligned() cil managed {
    .maxstack 2
    .locals init (int32 v)
    ldc.i4 123456
    stloc v
    ldloca v
    unaligned. 1
    ldind.i4
    ldc.i4 123456
    ceq
    ret
  }
  .method static int32 Id(int32 x) cil managed {
    .maxstack 1
    ldarg.0
    ret
  }
  .method static int32 TestTail() cil managed {
    .maxstack 1
    ldc.i4 9
    tail.
    call int32 P::Id(int32)
    ret
  }

  // ---- cpobj
  .method static int32 TestCpobjInt() cil managed {
    .maxstack 2
    .locals init (int32 a, int32 b)
    ldc.i4 77
    stloc a
    ldloca b
    ldloca a
    cpobj int32
    ldloc b
    ldc.i4 77
    ceq
    ret
  }
  .method static int32 TestCpobjLong() cil managed {
    .maxstack 2
    .locals init (int64 a, int64 b)
    ldc.i8 0x1122334455667788
    stloc a
    ldloca b
    ldloca a
    cpobj int64
    ldloc b
    ldc.i8 0x1122334455667788
    ceq
    ret
  }
  .method static int32 TestCpobjStruct() cil managed {
    .maxstack 3
    .locals init (valuetype Pair s, valuetype Pair t)
    ldloca s
    ldc.i4 -5
    stfld int32 Pair::a
    ldloca s
    ldc.i8 0x7fffffffffff
    stfld int64 Pair::b
    ldloca t
    ldloca s
    cpobj Pair
    ldloc t
    ldfld int32 Pair::a
    ldc.i4 -5
    bne.un FAIL
    ldloc t
    ldfld int64 Pair::b
    ldc.i8 0x7fffffffffff
    bne.un FAIL
    ldc.i4.1
    ret
    FAIL: ldc.i4.0
    ret
  }
  .method static int32 TestCpobjRef() cil managed {
    .maxstack 2
    .locals init (string a, string b)
    ldstr "hello"
    stloc a
    ldloca b
    ldloca a
    cpobj [@ASM@]System.String
    ldloc b
    ldloc a
    ceq
    ret
  }
  // byte[] element -> byte[] element: exactly one byte may move, never the neighbours
  .method static int32 TestCpobjByteArray() cil managed {
    .maxstack 4
    .locals init (uint8[] a, uint8[] b)
    ldc.i4.4
    newarr [@ASM@]System.Byte
    stloc a
    ldc.i4.4
    newarr [@ASM@]System.Byte
    stloc b
    ldloc a
    ldc.i4.0
    ldc.i4 200
    stelem.i1
    ldloc a
    ldc.i4.1
    ldc.i4 201
    stelem.i1
    ldloc b
    ldc.i4.0
    ldc.i4 1
    stelem.i1
    ldloc b
    ldc.i4.1
    ldc.i4 2
    stelem.i1
    ldloc b
    ldc.i4.2
    ldc.i4 3
    stelem.i1
    ldloc b
    ldc.i4.1
    ldelema [@ASM@]System.Byte
    ldloc a
    ldc.i4.0
    ldelema [@ASM@]System.Byte
    cpobj [@ASM@]System.Byte
    ldloc b
    ldc.i4.0
    ldelem.u1
    ldc.i4 1
    bne.un FAIL
    ldloc b
    ldc.i4.1
    ldelem.u1
    ldc.i4 200
    bne.un FAIL
    ldloc b
    ldc.i4.2
    ldelem.u1
    ldc.i4 3
    bne.un FAIL
    ldc.i4.1
    ret
    FAIL: ldc.i4.0
    ret
  }
  // int16 array element -> int16 local (negative value must stay negative and 16 bits wide)
  .method static int32 TestCpobjShort() cil managed {
    .maxstack 4
    .locals init (int16[] a, int16 v)
    ldc.i4.3
    newarr [@ASM@]System.Int16
    stloc a
    ldloc a
    ldc.i4.1
    ldc.i4 -300
    stelem.i2
    ldloca v
    ldloc a
    ldc.i4.1
    ldelema [@ASM@]System.Int16
    cpobj [@ASM@]System.Int16
    ldloc v
    ldc.i4 -300
    ceq
    ret
  }

  // ---- ckfinite: 0 = passed through, 1 = OverflowException, 2 = any other ArithmeticException
  .method static int32 Check(float64 v) cil managed {
    .maxstack 2
    .locals init (int32 r)
    .try {
      ldarg.0
      ckfinite
      pop
      ldc.i4.0
      stloc r
      leave DONE
    } catch [@ASM@]System.OverflowException {
      pop
      ldc.i4.1
      stloc r
      leave DONE
    } catch [@ASM@]System.ArithmeticException {
      pop
      ldc.i4.2
      stloc r
      leave DONE
    }
    DONE: ldloc r
    ret
  }
  .method static int32 CheckF32(float32 v) cil managed {
    .maxstack 2
    .locals init (int32 r)
    .try {
      ldarg.0
      ckfinite
      pop
      ldc.i4.0
      stloc r
      leave DONE
    } catch [@ASM@]System.OverflowException {
      pop
      ldc.i4.1
      stloc r
      leave DONE
    } catch [@ASM@]System.ArithmeticException {
      pop
      ldc.i4.2
      stloc r
      leave DONE
    }
    DONE: ldloc r
    ret
  }
  // five results as base-3 digits: 1.5, NaN, +Inf, -Inf, 0.0  -> Mono gives 0*81+1*27+1*9+1*3+0 = 39
  .method static int32 TestCkfiniteF64() cil managed {
    .maxstack 6
    ldc.r8 1.5
    call int32 P::Check(float64)
    ldc.i4 81
    mul
    ldc.r8 0.0
    ldc.r8 0.0
    div
    call int32 P::Check(float64)
    ldc.i4 27
    mul
    add
    ldc.r8 1.0
    ldc.r8 0.0
    div
    call int32 P::Check(float64)
    ldc.i4 9
    mul
    add
    ldc.r8 -1.0
    ldc.r8 0.0
    div
    call int32 P::Check(float64)
    ldc.i4.3
    mul
    add
    ldc.r8 0.0
    call int32 P::Check(float64)
    add
    ldc.i4 39
    ceq
    ret
  }
  .method static int32 TestCkfiniteF32() cil managed {
    .maxstack 6
    ldc.r4 1.5
    call int32 P::CheckF32(float32)
    ldc.i4 81
    mul
    ldc.r4 0.0
    ldc.r4 0.0
    div
    call int32 P::CheckF32(float32)
    ldc.i4 27
    mul
    add
    ldc.r4 1.0
    ldc.r4 0.0
    div
    call int32 P::CheckF32(float32)
    ldc.i4 9
    mul
    add
    ldc.r4 -1.0
    ldc.r4 0.0
    div
    call int32 P::CheckF32(float32)
    ldc.i4.3
    mul
    add
    ldc.r4 0.0
    call int32 P::CheckF32(float32)
    add
    ldc.i4 39
    ceq
    ret
  }
  // ckfinite leaves a finite value on the stack, unchanged
  .method static int32 TestCkfiniteKeepsValue() cil managed {
    .maxstack 2
    ldc.r8 2.5
    ckfinite
    ldc.r8 2.5
    ceq
    ret
  }

  // ---- sizeof: the reference runtime's layout for unmanaged value types
  .method static int32 TestSizeof() cil managed {
    .maxstack 2
    sizeof int32
    ldc.i4.4
    bne.un FAIL
    sizeof int64
    ldc.i4.8
    bne.un FAIL
    sizeof Pair
    ldc.i4.s 16
    bne.un FAIL
    sizeof Small
    ldc.i4.4
    bne.un FAIL
    ldc.i4.1
    ret
    FAIL: ldc.i4.0
    ret
  }

  .method public static int32 Main() cil managed {
    .entrypoint
    .maxstack 2
    call int32 P::TestBreak()
    brfalse F1
    call int32 P::TestVolatile()
    brfalse F2
    call int32 P::TestUnaligned()
    brfalse F3
    call int32 P::TestTail()
    ldc.i4 9
    bne.un F4
    call int32 P::TestCpobjInt()
    brfalse F5
    call int32 P::TestCpobjLong()
    brfalse F6
    call int32 P::TestCpobjStruct()
    brfalse F7
    call int32 P::TestCpobjRef()
    brfalse F8
    call int32 P::TestCpobjByteArray()
    brfalse F9
    call int32 P::TestCpobjShort()
    brfalse F10
    call int32 P::TestCkfiniteF64()
    brfalse F11
    call int32 P::TestCkfiniteF32()
    brfalse F12
    call int32 P::TestCkfiniteKeepsValue()
    brfalse F13
    call int32 P::TestSizeof()
    brfalse F14
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
    F6: ldc.i4.6
    ret
    F7: ldc.i4.7
    ret
    F8: ldc.i4.8
    ret
    F9: ldc.i4.s 9
    ret
    F10: ldc.i4.s 10
    ret
    F11: ldc.i4.s 11
    ret
    F12: ldc.i4.s 12
    ret
    F13: ldc.i4.s 13
    ret
    F14: ldc.i4.s 14
    ret
  }
}
