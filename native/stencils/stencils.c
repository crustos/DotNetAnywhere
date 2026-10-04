// Stencils: machine-code templates for the interpreter's simplest instructions. tools/gen_stencils.py compiles this
// file, reads the object code and its relocations, and writes native/src/Stencils.gen.h. At run time (NativeBlocks.c) a
// run of instructions becomes a block of native code by copying one stencil per instruction, one after the other, and
// patching each stencil's holes (a local's offset, a constant) with that instruction's operands.
//
// Rules, which the generator checks:
//  * each stencil is straight-line code (no jumps or calls) that ends in `ret`; the ret is dropped, so stencils fall
//    through into the next one;
//  * the only relocations are against HOLE0 / HOLE1: absolute 32-bit ones (a displacement or an immediate) against
//    either, and PC-relative branches against HOLE1, which is where a stencil that can fail jumps to (the block's exit
//    stub; see NativeBlocks.c). Such a stencil is written in assembly so that the jump is exactly where it is meant to
//    be: compiled from C, gcc moves the failure path after the `ret`, which is dropped;
//  * the evaluation stack pointer lives in r12 and the frame (the parameters and locals) in r13, in every stencil
//    (the file is compiled with -ffixed-r12 -ffixed-r13), so they can follow each other without a call convention.
//
// Each stencil does exactly what the interpreter's handler for that instruction does to the same memory, so a block is
// observably the same as interpreting the instructions one by one: the evaluation stack is still in memory, so the
// garbage collector and everything else see what they always did.

typedef unsigned int U32;

register unsigned char *SP asm("r12");     // the evaluation stack pointer
register unsigned char *FP asm("r13");     // the frame: parameters, then locals

extern char HOLE0[];
extern char HOLE1[];
#define H0 ((long)HOLE0)

// ldloc / ldarg of a 4-byte value (an int or a float: just 4 bytes)      H0 = byte offset in the frame
void st_ldl(void) { *(U32*)SP = *(U32*)(FP + H0); SP += 4; }
// stloc / starg of a 4-byte value
void st_stl(void) { SP -= 4; *(U32*)(FP + H0) = *(U32*)SP; }
// ldc.i4 / ldc.r4                                                         H0 = the 32 bits
void st_ldc(void) { *(U32*)SP = (U32)H0; SP += 4; }

// float32 arithmetic on the two values on top of the stack
void st_fadd(void) { SP -= 4; *(float*)(SP - 4) = *(float*)(SP - 4) + *(float*)SP; }
void st_fsub(void) { SP -= 4; *(float*)(SP - 4) = *(float*)(SP - 4) - *(float*)SP; }
void st_fmul(void) { SP -= 4; *(float*)(SP - 4) = *(float*)(SP - 4) * *(float*)SP; }
void st_fdiv(void) { SP -= 4; *(float*)(SP - 4) = *(float*)(SP - 4) / *(float*)SP; }
// neg on a float32: flip the sign bit (as an integer, so that no constant has to be loaded from memory)
void st_fneg(void) { *(U32*)(SP - 4) ^= 0x80000000u; }

// ---- pointers and objects. A reference or a managed pointer is 8 bytes (these stencils are for x86-64 only).

// ldloca / ldarga: push the address of a frame slot                       H0 = byte offset in the frame
void st_lda(void) { *(unsigned char**)SP = FP + H0; SP += 8; }
// ldloc / ldarg and stloc / starg of an 8-byte value (an object reference, a pointer)
void st_ldp(void) { *(unsigned long*)SP = *(unsigned long*)(FP + H0); SP += 8; }
void st_stp(void) { SP -= 8; *(unsigned long*)(FP + H0) = *(unsigned long*)SP; }
// dup of a 4-byte and of an 8-byte value
void st_dup4(void) { *(U32*)SP = *(U32*)(SP - 4); SP += 4; }
void st_dup8(void) { *(unsigned long*)SP = *(unsigned long*)(SP - 8); SP += 8; }

// ldfld of a field of up to 4 bytes: pop the object (or address), push the field.    H0 = the field's offset
// A null object jumps to HOLE1, the block's exit stub, which makes the interpreter throw NullReferenceException.
void st_ldfld4(void) {
	__asm__ volatile(
		"sub $8, %%r12\n\t"
		"mov (%%r12), %%rax\n\t"
		"test %%rax, %%rax\n\t"
		"jz HOLE1\n\t"
		"mov HOLE0(%%rax), %%eax\n\t"
		"mov %%eax, (%%r12)\n\t"
		"add $4, %%r12"
		::: "rax", "memory", "cc");
}
// stfld of a 4-byte value: pop the value and then the object, store the value in the field.   H0 = the field's offset
void st_stfld4(void) {
	__asm__ volatile(
		"sub $12, %%r12\n\t"
		"mov (%%r12), %%rax\n\t"
		"test %%rax, %%rax\n\t"
		"jz HOLE1\n\t"
		"mov 8(%%r12), %%edx\n\t"
		"mov %%edx, HOLE0(%%rax)"
		::: "rax", "rdx", "memory", "cc");
}

// ---- 32-bit integer arithmetic on the two ints on top of the stack (the same expressions as the interpreter's handlers)
typedef int I32;
void st_iadd(void)  { SP -= 4; *(U32*)(SP - 4) = *(U32*)(SP - 4) + *(U32*)SP; }
void st_isub(void)  { SP -= 4; *(U32*)(SP - 4) = *(U32*)(SP - 4) - *(U32*)SP; }
void st_imul(void)  { SP -= 4; *(U32*)(SP - 4) = *(U32*)(SP - 4) * *(U32*)SP; }
void st_iand(void)  { SP -= 4; *(U32*)(SP - 4) = *(U32*)(SP - 4) & *(U32*)SP; }
void st_ior(void)   { SP -= 4; *(U32*)(SP - 4) = *(U32*)(SP - 4) | *(U32*)SP; }
void st_ixor(void)  { SP -= 4; *(U32*)(SP - 4) = *(U32*)(SP - 4) ^ *(U32*)SP; }
void st_ishl(void)  { SP -= 4; *(U32*)(SP - 4) = *(U32*)(SP - 4) << *(U32*)SP; }
void st_ishr(void)  { SP -= 4; *(I32*)(SP - 4) = *(I32*)(SP - 4) >> *(U32*)SP; }
void st_ishrun(void){ SP -= 4; *(U32*)(SP - 4) = *(U32*)(SP - 4) >> *(U32*)SP; }

// ---- branches. Each is a jump to HOLE1, which the block compiler points at another stencil in the same block, at
// the block's exit for that branch, or (for a backward branch) at a trampoline that counts the iteration first.
// The comparisons are on the two ints on top of the stack, the deeper one first, as in the interpreter.
#define COMPARE_BRANCH(name, jcc) \
	void st_##name(void) { __asm__ volatile("sub $8, %%r12\n\tmov (%%r12), %%eax\n\tcmp 4(%%r12), %%eax\n\t" jcc " HOLE1" ::: "rax", "memory", "cc"); }
COMPARE_BRANCH(jeq, "je")
COMPARE_BRANCH(jge, "jge")
COMPARE_BRANCH(jgt, "jg")
COMPARE_BRANCH(jle, "jle")
COMPARE_BRANCH(jlt, "jl")
COMPARE_BRANCH(jne, "jne")
void st_jt(void) { __asm__ volatile("sub $4, %%r12\n\tcmpl $0, (%%r12)\n\tjne HOLE1" ::: "memory", "cc"); }   // brtrue
void st_jf(void) { __asm__ volatile("sub $4, %%r12\n\tcmpl $0, (%%r12)\n\tje HOLE1" ::: "memory", "cc"); }    // brfalse
void st_j(void)  { __asm__ volatile("jmp HOLE1"); }                                                              // br

// ---- float32 compare-and-branch: the two floats on top of the stack, the deeper one (a) first, as in the interpreter.
// ucomiss sets ZF, PF, CF to 1,1,1 if either operand is NaN, so "above" (CF=0 and ZF=0) and "above or equal" (CF=0) are false
// for NaN, which is what the ordered comparisons need, and "below or equal" / "below" are true for NaN, which is what the
// unordered ones (blt.un, ...) need. They compare b with a, or a with b, so that the one wanted is the one that is "above".
//   ordered:    a <  b : b above a             a <= b : b above-or-equal a     a >  b : a above b      a >= b : a above-or-equal b
//   unordered:  a <un b  = !(a >= b) : a below b        a >=un b = !(a < b)  : b below-or-equal a
//               a <=un b = !(a > b)  : a below-or-equal b    a >un b  = !(a <= b) : b below a
#define FLOAT_BRANCH_BA(name, jcc)  /* compares b with a */ \
	void st_##name(void) { __asm__ volatile("sub $8, %%r12\n\tmovss 4(%%r12), %%xmm1\n\tucomiss (%%r12), %%xmm1\n\t" jcc " HOLE1" ::: "xmm1", "memory", "cc"); }
#define FLOAT_BRANCH_AB(name, jcc)  /* compares a with b */ \
	void st_##name(void) { __asm__ volatile("sub $8, %%r12\n\tmovss (%%r12), %%xmm0\n\tucomiss 4(%%r12), %%xmm0\n\t" jcc " HOLE1" ::: "xmm0", "memory", "cc"); }
FLOAT_BRANCH_BA(jflt, "ja")        // a <  b
FLOAT_BRANCH_BA(jfle, "jae")       // a <= b
FLOAT_BRANCH_AB(jfgt, "ja")        // a >  b
FLOAT_BRANCH_AB(jfge, "jae")       // a >= b
FLOAT_BRANCH_AB(jflt_un, "jb")     // blt.un: !(a >= b)
FLOAT_BRANCH_BA(jfge_un, "jbe")    // bge.un: !(a < b)
FLOAT_BRANCH_AB(jfle_un, "jbe")    // ble.un: !(a > b)
FLOAT_BRANCH_BA(jfgt_un, "jb")     // bgt.un: !(a <= b)
// a == b (false for NaN): equal and not unordered
void st_jfeq(void) { __asm__ volatile("sub $8, %%r12\n\tmovss (%%r12), %%xmm0\n\tucomiss 4(%%r12), %%xmm0\n\tsete %%al\n\tsetnp %%cl\n\ttest %%cl, %%al\n\tjnz HOLE1" ::: "xmm0", "rax", "rcx", "memory", "cc"); }
// a != b (true for NaN)
void st_jfne(void) { __asm__ volatile("sub $8, %%r12\n\tmovss (%%r12), %%xmm0\n\tucomiss 4(%%r12), %%xmm0\n\tjne HOLE1\n\tjp HOLE1" ::: "xmm0", "memory", "cc"); }

// ---- conversions between int32 and float32
void st_cvtif(void) { *(float*)(SP - 4) = (float)*(int*)(SP - 4); }                        // conv.r4 of an int
// conv.i4 (and, with a shift, conv.i1 / conv.i2) of a float: the interpreter's expression       H0 = the shift
void st_cvtfi(void) { int r = (int)*(float*)(SP - 4); r = (r << H0) >> H0; *(int*)(SP - 4) = r; }
