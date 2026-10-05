// The structs the FFI tests pass by value and by pointer (tests/ffi/mylib.json declares them in "structs").
#ifndef MYLIB_H
#define MYLIB_H
#include <stdint.h>
typedef struct { int32_t a; float b; } Pair;                              // 8 bytes: one register
typedef struct { int32_t r, g, b, a; } Rgba;                              // 16 bytes: two registers
typedef struct { double x, y, z; } V3;                                    // 24 bytes: passed in memory
typedef struct { uint32_t id; float x, y, angle; int32_t mode; } Xf;      // 20 bytes: like a transform record
typedef struct { int32_t s; int64_t l; float f; } Padded;                 // 24 bytes: padding after s and after f
typedef struct { int32_t a; uint8_t b; } Tail;                            // 8 bytes in C (tail padding), 5 in a C# struct: a layout that must be refused
typedef struct { uint8_t r, g, b, a; } Bytes;                             // 4 bytes in C, 16 in a C# struct (a narrow field has a 4-byte slot there): must be refused
// callbacks: C function pointers that a C# delegate stands behind (valid only for the call they are passed to)
typedef int (*binop_fn)(int, int);
typedef void (*visit_fn)(int, double);
typedef double (*mapd_fn)(double);
typedef int64_t (*acc_fn)(int64_t, float, void*);
typedef int (*cmp_fn)(const void*, const void*);
#endif
