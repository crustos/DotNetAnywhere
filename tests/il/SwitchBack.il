// A switch whose targets include the loop header itself (a backward edge straight out of the switch): not something C# compilers emit, and not
// something a structured loop can express with one br_table, so the wasm JIT has to give up structured code for it and use its dispatch loop.
// Each method returns a value that Main checks; Main returns the number of the first failing check, or 0.

.assembly extern @ASM@ { }
.assembly SwitchBack { }

.class public auto ansi beforefieldinit P extends [@ASM@]System.Object {
  // i % 3 selects: 0 -> H (back), 1 -> X1, 2 -> H (back). Expected 2021.
  .method public static int32 F() cil managed {
    .maxstack 3
    .locals init (int32 i, int32 acc)
    H:  ldloc i
        ldc.i4.1
        add
        stloc i
        ldloc acc
        ldloc i
        add
        stloc acc
        ldloc i
        ldc.i4.6
        blt CONT
        ldloc acc
        ret
    CONT: ldloc i
        ldc.i4.3
        rem
        switch (H, X1, H)
    D:  ldloc acc
        ldc.i4 100
        add
        stloc acc
        br H
    X1: ldloc acc
        ldc.i4 1000
        add
        stloc acc
        br H
  }
  // i % 4 selects: 0 -> H (back), 1 -> X1, 2 -> H (back), 3 -> not in the table: falls through to D. Expected 2121.
  .method public static int32 G() cil managed {
    .maxstack 3
    .locals init (int32 i, int32 acc)
    H:  ldloc i
        ldc.i4.1
        add
        stloc i
        ldloc acc
        ldloc i
        add
        stloc acc
        ldloc i
        ldc.i4.6
        blt CONT
        ldloc acc
        ret
    CONT: ldloc i
        ldc.i4.4
        rem
        switch (H, X1, H)
    D:  ldloc acc
        ldc.i4 100
        add
        stloc acc
        br H
    X1: ldloc acc
        ldc.i4 1000
        add
        stloc acc
        br H
  }
  .method public static int32 Main() cil managed {
    .entrypoint
    .maxstack 2
    ldstr "keeps Main interpreted, so that F and G are entry functions of the wasm JIT, which can give up the processor"
    pop
    call int32 P::F()
    ldc.i4 2021
    beq OK1
    ldc.i4.1
    ret
    OK1: call int32 P::G()
    ldc.i4 2121
    beq OK2
    ldc.i4.2
    ret
    OK2: ldc.i4.0
    ret
  }
}
