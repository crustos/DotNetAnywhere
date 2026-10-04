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
//    stub; see NativeBlocks.c) and, for array access, against HOLE2, the exit for an index out of range. Such a stencil is
//    written in assembly so that the jump is exactly where it is meant to
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
extern char HOLE2[];
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

// ---- arrays. An array object is its length (a 32-bit integer at offset 0), then its elements from offset 4
// (NativeBlocks.c asserts that this is the layout). A null array jumps to HOLE1 (NullReferenceException), an index that is
// not below the length, as an unsigned number so that a negative one is too, to HOLE2 (IndexOutOfRangeException).
// The stack is [array (8 bytes)] [index (4)] and, for a store, [value (4)] above that.
#define ARRAY_PROLOGUE(pop) \
	"sub $" #pop ", %%r12\n\t" \
	"mov (%%r12), %%rax\n\t" \
	"test %%rax, %%rax\n\t" \
	"jz HOLE1\n\t" \
	"mov 8(%%r12), %%edx\n\t" \
	"cmp (%%rax), %%edx\n\t" \
	"jae HOLE2\n\t"
// ldelem.i4 / .u4 / .r4: push the 4-byte element
void st_ldelem4(void)  { __asm__ volatile(ARRAY_PROLOGUE(12) "mov 4(%%rax,%%rdx,4), %%eax\n\tmov %%eax, (%%r12)\n\tadd $4, %%r12" ::: "rax", "rdx", "memory", "cc"); }
// ldelem.u1 of a byte[] element: push the byte, zero-extended
void st_ldelemu1(void) { __asm__ volatile(ARRAY_PROLOGUE(12) "movzbl 4(%%rax,%%rdx,1), %%eax\n\tmov %%eax, (%%r12)\n\tadd $4, %%r12" ::: "rax", "rdx", "memory", "cc"); }
// ldelem.u1 of a bool[] element: the low byte of its 4-byte element, zero-extended
void st_ldelemb4(void) { __asm__ volatile(ARRAY_PROLOGUE(12) "movzbl 4(%%rax,%%rdx,4), %%eax\n\tmov %%eax, (%%r12)\n\tadd $4, %%r12" ::: "rax", "rdx", "memory", "cc"); }
// stelem.i4 / .r4 and stelem.i1: store the value (all 32 bits, or the low 8)
void st_stelem4(void)  { __asm__ volatile(ARRAY_PROLOGUE(16) "mov 12(%%r12), %%ecx\n\tmov %%ecx, 4(%%rax,%%rdx,4)" ::: "rax", "rcx", "rdx", "memory", "cc"); }
void st_stelem1(void)  { __asm__ volatile(ARRAY_PROLOGUE(16) "mov 12(%%r12), %%ecx\n\tmov %%cl, 4(%%rax,%%rdx,1)" ::: "rax", "rcx", "rdx", "memory", "cc"); }
// ldelema: push the address of the element                                  H0 = the element size
void st_ldelema(void)  { __asm__ volatile(ARRAY_PROLOGUE(12) "imul $HOLE0, %%rdx, %%rdx\n\tlea 4(%%rax,%%rdx), %%rax\n\tmov %%rax, (%%r12)\n\tadd $8, %%r12" ::: "rax", "rdx", "memory", "cc"); }
// ldlen: push the length (a null array jumps to HOLE1)
void st_ldlen(void) {
	__asm__ volatile("sub $8, %%r12\n\tmov (%%r12), %%rax\n\ttest %%rax, %%rax\n\tjz HOLE1\n\tmov (%%rax), %%eax\n\tmov %%eax, (%%r12)\n\tadd $4, %%r12"
		::: "rax", "memory", "cc");
}

// ---- narrowing conversions of an int32, in place, the interpreter's expressions.
// conv.i4 / conv.i1 / conv.i2 of an int:  (v << shift) >> shift     H0 = the shift (0 for conv.i4: C# puts one after every .Length)
void st_cvtii(void)   { int r = *(int*)(SP - 4); *(int*)(SP - 4) = (r << H0) >> H0; }
// conv.u1 / conv.u2 / conv.u4 of an int:  v & mask                    H0 = the mask
void st_cvtmask(void) { *(U32*)(SP - 4) &= (U32)H0; }

// =====================================================================================================================
// 64-bit integers and doubles. A long or a double takes 8 bytes of evaluation stack, little-endian, like everything else here.
// =====================================================================================================================
typedef unsigned long long U64;
typedef long long I64;

// ldc.i8 / ldc.r8: the 64 bits are two 32-bit holes, so it is two stencils, always emitted together: the first puts the low half
// in eax, the second builds the whole value and stores it with ONE 8-byte store. (Two 4-byte stores, as this was first written,
// are slow: the next stencil reads the value with an 8-byte load, which cannot be forwarded from two stores, and stalls.)
void st_ldc8lo(void) { __asm__ volatile("mov $HOLE0, %%eax" ::: "rax"); }
void st_ldc8hi(void) { __asm__ volatile("mov $HOLE0, %%edx\n\tshl $32, %%rdx\n\tor %%rdx, %%rax\n\tmov %%rax, (%%r12)\n\tadd $8, %%r12" ::: "rax", "rdx", "memory", "cc"); }

// ---- long arithmetic on the two longs on top of the stack
#define LONG_OP(name, op) void st_##name(void) { SP -= 8; *(U64*)(SP - 8) = *(U64*)(SP - 8) op *(U64*)SP; }
LONG_OP(ladd, +)
LONG_OP(lsub, -)
LONG_OP(lmul, *)
LONG_OP(land, &)
LONG_OP(lor, |)
LONG_OP(lxor, ^)
// shifts: the count is an int on top of a long (the interpreter's BINARY_OP(U64, U64, U32, <<))
void st_lshl(void)   { SP -= 4; *(U64*)(SP - 8) = *(U64*)(SP - 8) << *(U32*)SP; }
void st_lshr(void)   { SP -= 4; *(I64*)(SP - 8) = *(I64*)(SP - 8) >> *(U32*)SP; }
void st_lshrun(void) { SP -= 4; *(U64*)(SP - 8) = *(U64*)(SP - 8) >> *(U32*)SP; }
void st_lneg(void)   { *(I64*)(SP - 8) = -*(I64*)(SP - 8); }

// ---- double arithmetic
#define DOUBLE_OP(name, op) void st_##name(void) { SP -= 8; *(double*)(SP - 8) = *(double*)(SP - 8) op *(double*)SP; }
DOUBLE_OP(dadd, +)
DOUBLE_OP(dsub, -)
DOUBLE_OP(dmul, *)
DOUBLE_OP(ddiv, /)
void st_dneg(void) { *(U64*)(SP - 8) ^= 0x8000000000000000ull; }     // flip the sign bit

// ---- conversions that change the size of what is on the stack
void st_cvtil(void) { *(I64*)(SP - 4) = (I64)*(int*)(SP - 4); SP += 4; }               // conv.i8 of an int: sign-extend
void st_cvtul(void) { *(U64*)(SP - 4) = (U64)*(U32*)(SP - 4); SP += 4; }               // conv.u8 / of a uint: zero-extend
void st_cvtli(void) { int v = (int)*(U64*)(SP - 8); SP -= 4; *(int*)(SP - 4) = (v << H0) >> H0; }   // conv.i4 etc. of a long   H0 = shift
void st_cvtid(void) { *(double*)(SP - 4) = (double)*(int*)(SP - 4); SP += 4; }         // conv.r8 of an int
void st_cvtdi(void) { int r = (int)*(double*)(SP - 8); SP -= 4; *(int*)(SP - 4) = (r << H0) >> H0; } // conv.i4 etc. of a double   H0 = shift
void st_cvtfd(void) { *(double*)(SP - 4) = (double)*(float*)(SP - 4); SP += 4; }       // float to double
void st_cvtdf(void) { float f = (float)*(double*)(SP - 8); SP -= 4; *(float*)(SP - 4) = f; }   // double to float

// ---- long compare-and-branch: the two longs on top of the stack, the deeper one (a) first; signed, as the interpreter does
#define LONG_BRANCH(name, jcc) \
	void st_##name(void) { __asm__ volatile("sub $16, %%r12\n\tmov (%%r12), %%rax\n\tcmp 8(%%r12), %%rax\n\t" jcc " HOLE1" ::: "rax", "memory", "cc"); }
LONG_BRANCH(lbeq, "je")
LONG_BRANCH(lbge, "jge")
LONG_BRANCH(lbgt, "jg")
LONG_BRANCH(lble, "jle")
LONG_BRANCH(lblt, "jl")
LONG_BRANCH(lbne, "jne")

// ---- double compare-and-branch: as the float32 ones, with ucomisd (see there for the reasoning about NaN)
#define DOUBLE_BRANCH_BA(name, jcc) \
	void st_##name(void) { __asm__ volatile("sub $16, %%r12\n\tmovsd 8(%%r12), %%xmm1\n\tucomisd (%%r12), %%xmm1\n\t" jcc " HOLE1" ::: "xmm1", "memory", "cc"); }
#define DOUBLE_BRANCH_AB(name, jcc) \
	void st_##name(void) { __asm__ volatile("sub $16, %%r12\n\tmovsd (%%r12), %%xmm0\n\tucomisd 8(%%r12), %%xmm0\n\t" jcc " HOLE1" ::: "xmm0", "memory", "cc"); }
DOUBLE_BRANCH_BA(jdlt, "ja")
DOUBLE_BRANCH_BA(jdle, "jae")
DOUBLE_BRANCH_AB(jdgt, "ja")
DOUBLE_BRANCH_AB(jdge, "jae")
DOUBLE_BRANCH_AB(jdlt_un, "jb")
DOUBLE_BRANCH_BA(jdge_un, "jbe")
DOUBLE_BRANCH_AB(jdle_un, "jbe")
DOUBLE_BRANCH_BA(jdgt_un, "jb")
void st_jdeq(void) { __asm__ volatile("sub $16, %%r12\n\tmovsd (%%r12), %%xmm0\n\tucomisd 8(%%r12), %%xmm0\n\tsete %%al\n\tsetnp %%cl\n\ttest %%cl, %%al\n\tjnz HOLE1" ::: "xmm0", "rax", "rcx", "memory", "cc"); }
void st_jdne(void) { __asm__ volatile("sub $16, %%r12\n\tmovsd (%%r12), %%xmm0\n\tucomisd 8(%%r12), %%xmm0\n\tjne HOLE1\n\tjp HOLE1" ::: "xmm0", "memory", "cc"); }

// ---- for inlining a small method into a block (see FuseOps in JIT.c)
// Clear 4 or 8 bytes of the frame, to give an inlined method's locals the zero a real call would                H0 = offset in the frame
void st_zero4(void) { *(U32*)(FP + H0) = 0; }
void st_zero8(void) { *(unsigned long*)(FP + H0) = 0; }
// callvirt of a method that is not virtual still has to check `this`, which is (the size of the arguments) below the top of the stack:
// the reference at that depth must not be null (a null one jumps to HOLE1, the NullReferenceException exit)      H0 = minus that depth
void st_chkthis(void) { __asm__ volatile("mov HOLE0(%%r12), %%rax\n\ttest %%rax, %%rax\n\tjz HOLE1" ::: "rax", "cc"); }

// ---- 8-byte fields (long, double, references), and the address of a field
// ldfld of an 8-byte field: the object (or address) is replaced by the field's value.   H0 = the field's offset;  null: HOLE1
void st_ldfld8(void) {
	__asm__ volatile(
		"mov -8(%%r12), %%rax\n\t"
		"test %%rax, %%rax\n\t"
		"jz HOLE1\n\t"
		"mov HOLE0(%%rax), %%rax\n\t"
		"mov %%rax, -8(%%r12)"
		::: "rax", "memory", "cc");
}
// stfld of an 8-byte value: pop the value and then the object.                          H0 = the field's offset;  null: HOLE1
void st_stfld8(void) {
	__asm__ volatile(
		"sub $16, %%r12\n\t"
		"mov (%%r12), %%rax\n\t"
		"test %%rax, %%rax\n\t"
		"jz HOLE1\n\t"
		"mov 8(%%r12), %%rdx\n\t"
		"mov %%rdx, HOLE0(%%rax)"
		::: "rax", "rdx", "memory", "cc");
}
// ldflda: the object becomes the address of the field                                   H0 = the field's offset;  null: HOLE1
void st_ldflda(void) {
	__asm__ volatile(
		"mov -8(%%r12), %%rax\n\t"
		"test %%rax, %%rax\n\t"
		"jz HOLE1\n\t"
		"lea HOLE0(%%rax), %%rax\n\t"
		"mov %%rax, -8(%%r12)"
		::: "rax", "memory", "cc");
}

// ---- long <-> float and double (the conversions are the C ones, as the interpreter's handlers are)
void st_cvtld(void) { *(double*)(SP - 8) = (double)*(I64*)(SP - 8); }                 // conv.r8 of a long
void st_cvtlf(void) { *(float*)(SP - 8) = (float)*(I64*)(SP - 8); SP -= 4; }          // conv.r4 of a long
void st_cvtdl(void) { *(I64*)(SP - 8) = (I64)*(double*)(SP - 8); }                    // conv.i8 of a double
void st_cvtfl(void) { *(I64*)(SP - 4) = (I64)*(float*)(SP - 4); SP += 4; }            // conv.i8 of a float

// ---- brtrue / brfalse on a reference or pointer (8 bytes)
void st_jt8(void) { __asm__ volatile("sub $8, %%r12\n\tcmpq $0, (%%r12)\n\tjne HOLE1" ::: "memory", "cc"); }
void st_jf8(void) { __asm__ volatile("sub $8, %%r12\n\tcmpq $0, (%%r12)\n\tje HOLE1" ::: "memory", "cc"); }

// ---- the register stencils (three-address, with a virtual evaluation stack: see tools/gen_vstencils.py and the register pass in JIT.c)
#include "vstencils.gen.c"
