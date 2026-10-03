// calli (call through a method pointer) and jmp (transfer to another method with the same
// arguments), which C# compilers do not emit. @ASM@ is the core library's name.
// Each Test* method returns 1 on success; Main returns the number of the first failing test.
// (A calli through a NULL pointer is not tested here: Mono aborts on it. See CalliNull.dnaonly.il.)

.assembly extern @ASM@ { }
.assembly CallsAndJumps { }

.class public auto ansi beforefieldinit Animal extends [@ASM@]System.Object {
  .field public int32 k
  .method public specialname rtspecialname instance void .ctor() cil managed {
    .maxstack 1
    ldarg.0
    call instance void [@ASM@]System.Object::.ctor()
    ret
  }
  .method public virtual instance int32 V() cil managed {
    .maxstack 1
    ldc.i4.1
    ret
  }
  .method public instance int32 AddK(int32 x) cil managed {
    .maxstack 2
    ldarg.0
    ldfld int32 Animal::k
    ldarg.1
    add
    ret
  }
  // jmp on an instance method: `this` and x are passed straight on to AddK
  .method public instance int32 JmpInst(int32 x) cil managed {
    jmp instance int32 Animal::AddK(int32)
  }
}
.class public auto ansi beforefieldinit Dog extends Animal {
  .method public specialname rtspecialname instance void .ctor() cil managed {
    .maxstack 1
    ldarg.0
    call instance void Animal::.ctor()
    ret
  }
  .method public virtual instance int32 V() cil managed {
    .maxstack 1
    ldc.i4.2
    ret
  }
}

.class public auto ansi beforefieldinit P extends [@ASM@]System.Object {
  .field public static int32 sf

  // ---- targets
  .method static int32 Add(int32 a, int32 b) cil managed {
    .maxstack 2
    ldarg.0
    ldarg.1
    add
    ret
  }
  .method static int32 Sub(int32 a, int32 b) cil managed {
    .maxstack 2
    ldarg.0
    ldarg.1
    sub
    ret
  }
  .method static int32 Mul(int32 a, int32 b) cil managed {
    .maxstack 2
    ldarg.0
    ldarg.1
    mul
    ret
  }
  .method static int64 Mix(int32 a, int64 b, float64 c) cil managed {
    .maxstack 3
    ldarg.0
    conv.i8
    ldarg.1
    add
    ldarg.2
    conv.i8
    add
    ret
  }
  .method static float64 Half(float64 x) cil managed {
    .maxstack 2
    ldarg.0
    ldc.r8 2.0
    div
    ret
  }
  .method static void SetSf(int32 v) cil managed {
    .maxstack 1
    ldarg.0
    stsfld int32 P::sf
    ret
  }
  .method static int32 Two() cil managed {
    .maxstack 1
    ldc.i4.2
    ret
  }

  // ---- calli
  .method static int32 TestCalliStatic() cil managed {
    .maxstack 3
    ldc.i4 30
    ldc.i4 12
    ldftn int32 P::Add(int32, int32)
    calli default int32 (int32, int32)
    ldc.i4 42
    ceq
    ret
  }
  .method static int32 TestCalliNoArgs() cil managed {
    .maxstack 1
    ldftn int32 P::Two()
    calli default int32 ()
    ldc.i4.2
    ceq
    ret
  }
  .method static int32 TestCalliWide() cil managed {
    .maxstack 4
    ldc.i4 1
    ldc.i8 0x100000000
    ldc.r8 3.9
    ldftn int64 P::Mix(int32, int64, float64)
    calli default int64 (int32, int64, float64)
    ldc.i8 0x100000004
    ceq
    ret
  }
  .method static int32 TestCalliDouble() cil managed {
    .maxstack 2
    ldc.r8 9.0
    ldftn float64 P::Half(float64)
    calli default float64 (float64)
    ldc.r8 4.5
    ceq
    ret
  }
  .method static int32 TestCalliVoid() cil managed {
    .maxstack 2
    ldc.i4.0
    stsfld int32 P::sf
    ldc.i4 55
    ldftn void P::SetSf(int32)
    calli default void (int32)
    ldsfld int32 P::sf
    ldc.i4 55
    ceq
    ret
  }
  // instance method: `this` is the first argument
  .method static int32 TestCalliInstance() cil managed {
    .maxstack 3
    .locals init (class Animal a)
    newobj instance void Animal::.ctor()
    stloc a
    ldloc a
    ldc.i4 100
    stfld int32 Animal::k
    ldloc a
    ldc.i4 23
    ldftn instance int32 Animal::AddK(int32)
    calli instance default int32 (int32)
    ldc.i4 123
    ceq
    ret
  }
  // ldvirtftn + calli = a virtual call done by hand
  .method static int32 TestCalliVirtual() cil managed {
    .maxstack 3
    .locals init (class Animal a)
    newobj instance void Dog::.ctor()
    stloc a
    ldloc a
    ldloc a
    ldvirtftn instance int32 Animal::V()
    calli instance default int32 ()
    ldc.i4.2
    bne.un FAIL
    newobj instance void Animal::.ctor()
    stloc a
    ldloc a
    ldloc a
    ldvirtftn instance int32 Animal::V()
    calli instance default int32 ()
    ldc.i4.1
    ceq
    ret
    FAIL: ldc.i4.0
    ret
  }
  // a table of function pointers, chosen at run time
  .method static int32 Apply(native int fn, int32 a, int32 b) cil managed {
    .maxstack 3
    ldarg.1
    ldarg.2
    ldarg.0
    calli default int32 (int32, int32)
    ret
  }
  .method static int32 TestCalliTable() cil managed {
    .maxstack 4
    ldftn int32 P::Add(int32, int32)
    ldc.i4 7
    ldc.i4 3
    call int32 P::Apply(native int, int32, int32)
    ldc.i4 10
    bne.un FAIL
    ldftn int32 P::Sub(int32, int32)
    ldc.i4 7
    ldc.i4 3
    call int32 P::Apply(native int, int32, int32)
    ldc.i4 4
    bne.un FAIL
    ldftn int32 P::Mul(int32, int32)
    ldc.i4 7
    ldc.i4 3
    call int32 P::Apply(native int, int32, int32)
    ldc.i4 21
    ceq
    ret
    FAIL: ldc.i4.0
    ret
  }
  // ---- jmp
  .method static int32 Target(int32 a, int32 b) cil managed {
    .maxstack 3
    ldarg.0
    ldc.i4 10
    mul
    ldarg.1
    add
    ret
  }
  .method static int32 JmpStatic(int32 a, int32 b) cil managed {
    jmp int32 P::Target(int32, int32)
  }
  .method static int64 Target3(int32 a, int64 b, float64 c) cil managed {
    .maxstack 4
    ldarg.0
    conv.i8
    ldarg.1
    add
    ldarg.2
    conv.i8
    add
    ret
  }
  .method static int64 JmpWide(int32 a, int64 b, float64 c) cil managed {
    jmp int64 P::Target3(int32, int64, float64)
  }
  .method static void TargetVoid(int32 v) cil managed {
    .maxstack 1
    ldarg.0
    ldc.i4 3
    mul
    stsfld int32 P::sf
    ret
  }
  .method static void JmpVoid(int32 v) cil managed {
    jmp void P::TargetVoid(int32)
  }
  .method static int32 TestJmpStatic() cil managed {
    .maxstack 2
    ldc.i4 4
    ldc.i4 2
    call int32 P::JmpStatic(int32, int32)
    ldc.i4 42
    ceq
    ret
  }
  .method static int32 TestJmpWide() cil managed {
    .maxstack 4
    ldc.i4 1
    ldc.i8 0x100000000
    ldc.r8 3.9
    call int64 P::JmpWide(int32, int64, float64)
    ldc.i8 0x100000004
    ceq
    ret
  }
  .method static int32 TestJmpVoid() cil managed {
    .maxstack 2
    ldc.i4.0
    stsfld int32 P::sf
    ldc.i4 9
    call void P::JmpVoid(int32)
    ldsfld int32 P::sf
    ldc.i4 27
    ceq
    ret
  }
  .method static int32 TestJmpInstance() cil managed {
    .maxstack 3
    .locals init (class Animal a)
    newobj instance void Animal::.ctor()
    stloc a
    ldloc a
    ldc.i4 100
    stfld int32 Animal::k
    ldloc a
    ldc.i4 5
    callvirt instance int32 Animal::JmpInst(int32)
    ldc.i4 105
    ceq
    ret
  }

  .method public static int32 Main() cil managed {
    .entrypoint
    .maxstack 2
    call int32 P::TestCalliStatic()
    brfalse F1
    call int32 P::TestCalliNoArgs()
    brfalse F2
    call int32 P::TestCalliWide()
    brfalse F3
    call int32 P::TestCalliDouble()
    brfalse F4
    call int32 P::TestCalliVoid()
    brfalse F5
    call int32 P::TestCalliInstance()
    brfalse F6
    call int32 P::TestCalliVirtual()
    brfalse F7
    call int32 P::TestCalliTable()
    brfalse F8
    call int32 P::TestJmpStatic()
    brfalse F9
    call int32 P::TestJmpWide()
    brfalse F10
    call int32 P::TestJmpVoid()
    brfalse F11
    call int32 P::TestJmpInstance()
    brfalse F12
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
  }
}
