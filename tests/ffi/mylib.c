// A small C library for the FFI tests and benchmark. Built into the runtime by build.py --ffi tests/ffi/mylib.json, and (for Mono, which
// is what the results are compared with) as a shared library.
#include <stdint.h>
#include <emmintrin.h>
#include <stdlib.h>
#include <string.h>

int add_numbers(int a, int b) { return a + b; }
int sum6(int a, int b, int c, int d, int e, int f) { return a + b * 2 + c * 3 + d * 4 + e * 5 + f * 6; }
int64_t mix64(int64_t a, int b) { return (a << 3) ^ (a + b); }
unsigned int uclamp(unsigned int v, unsigned int lo, unsigned int hi) { return v < lo ? lo : (v > hi ? hi : v); }
double hyp2(double a, double b) { return a * a + b * b; }
float scalef(float x, float k) { return x * k + 0.5f; }
double mixed(int a, double b, int64_t c, float d, int e) { return a + b * 2.0 + (double)c + d * 0.25 + e; }
int16_t narrow16(int v) { return (int16_t)v; }
uint8_t low8(int v) { return (uint8_t)v; }
void* ptr_bump(void* p, int n) { return (char*)p + n; }
static int counter;
void bump_counter(int by) { counter += by; }
int get_counter(void) { return counter; }
int bump_get(int by) { counter += by; return counter; }

// Needs the stack aligned at the call: an aligned local read with an aligned SSE load (the compiler relies on the ABI's alignment of rsp), so a
// call made with a misaligned stack faults here
double aligned_sum(double a, double b) {
	double v[4] __attribute__((aligned(16))) = { a, b, a * 2.0, b * 2.0 };
	__m128d x = _mm_load_pd(v), y = _mm_load_pd(v + 2);
	__m128d z = _mm_add_pd(x, y);
	double out[2] __attribute__((aligned(16)));
	_mm_store_pd(out, z);
	return out[0] + out[1];
}

// ---- arrays, ref and out arguments, strings
int sum_buf(const int32_t* p, int n) { int s = 0; for (int i = 0; i < n; i++) s += p[i] * (i + 1); return s; }
int64_t sum_i64(const int64_t* p, int n) { int64_t s = 0; for (int i = 0; i < n; i++) s += p[i] ^ (i * 7); return s; }
void fill_buf(int32_t* p, int n, int v) { for (int i = 0; i < n; i++) p[i] = v + i * 3; }
double dot(const double* a, const double* b, int n) { double s = 0; for (int i = 0; i < n; i++) s += a[i] * b[i]; return s; }
void upcase(unsigned char* b, int n) { for (int i = 0; i < n; i++) if (b[i] >= 'a' && b[i] <= 'z') b[i] -= 32; }
int is_null(const void* p) { return p == 0 ? 1 : 0; }
void swap_ref(int32_t* a, int32_t* b) { int32_t t = *a; *a = *b; *b = t; }
void divmod(int a, int b, int32_t* q, int32_t* r) { *q = a / b; *r = a % b; }
int str_len(const char* s) { return (int)strlen(s); }
int str_sum(const char* s) { int t = 0; for (; *s; s++) t = t * 31 + (unsigned char)*s; return t; }
// a string result is freed by the runtime (as .NET does), so it is malloc'd; NULL is a null string
char* greeting(int k) {
	static const char* const g[] = { "", "hello", "h\xc3\xa9llo w\xc3\xb6rld", "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e", "smile \xf0\x9f\x98\x80 done" };
	if (k < 0) return 0;
	return strdup(g[k % 5]);
}
char* echo_upper(const char* s) {
	if (!s) return 0;
	char* r = strdup(s);
	for (char* p = r; *p; p++) if (*p >= 'a' && *p <= 'z') *p -= 32;
	return r;
}

// ---- more than six arguments: past the registers a C call has them on the stack; a stencil cannot make such a call, so it goes through the wrapper
int sum9(int a, int b, int c, int d, int e, int f, int g, int h, int i) { return a + 2 * b + 3 * c + 4 * d + 5 * e + 6 * f + 7 * g + 8 * h + 9 * i; }
int64_t sum8l(int64_t a, int64_t b, int64_t c, int64_t d, int64_t e, int64_t f, int64_t g, int64_t h) { return a - 2 * b + 3 * c - 4 * d + 5 * e - 6 * f + 7 * g - 8 * h; }
double mix12(int a, double b, int64_t c, float d, int e, double f, int g, int64_t h, float i, int j, double k, int l) {
	return a + b * 2 + (double)c * 3 + d * 4 + e * 5 + f * 6 + g * 7 + (double)h * 8 + i * 9 + j * 10 + k * 11 + l * 12;
}
double sum10d(double a, double b, double c, double d, double e, double f, double g, double h, double i, double j) { return a + 2 * b + 3 * c + 4 * d + 5 * e + 6 * f + 7 * g + 8 * h + 9 * i + 10 * j; }
float sum10f(float a, float b, float c, float d, float e, float f, float g, float h, float i, float j) { return a + 2 * b + 3 * c + 4 * d + 5 * e + 6 * f + 7 * g + 8 * h + 9 * i + 10 * j; }
int wide_str(const char* s, int a, int b, int c, int d, int e, int f, int g) { return (int)strlen(s) * 1000 + a + b + c + d + e + f + g; }
int wide_buf(const int32_t* p, int n, int a, int b, int c, int d, int e, int f, int g) { int s = 0; for (int i = 0; i < n; i++) s += p[i]; return s + a * b - c * d + e * f - g; }
void wide_out(int a, int b, int c, int d, int e, int f, int g, int32_t* out, int32_t* out2) { *out = a + b + c + d + e + f + g; *out2 = a * b * c - d * e * f + g; }

// ---- structs by value, results, arrays and pointers
#include "mylib.h"
Pair pair_make(int a, float b) { Pair p; p.a = a; p.b = b; return p; }
int pair_sum(Pair p) { return p.a * 3 + (int)(p.b * 10); }
Pair pair_swap(Pair p, int k) { Pair r; r.a = (int)p.b + k; r.b = (float)p.a - k; return r; }
Rgba rgba_blend(Rgba x, Rgba y) { Rgba r; r.r = (x.r + y.r) / 2; r.g = (x.g + y.g) / 2; r.b = (x.b + y.b) / 2; r.a = x.a > y.a ? x.a : y.a; return r; }
V3 v3_add(V3 a, V3 b) { V3 r; r.x = a.x + b.x; r.y = a.y + b.y; r.z = a.z + b.z; return r; }
double v3_dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 v3_scale(V3 a, double k) { V3 r; r.x = a.x * k; r.y = a.y * k; r.z = a.z * k; return r; }
V3 v3_make(double x, double y, double z) { V3 r; r.x = x; r.y = y; r.z = z; return r; }
double v3_mix(V3 a, int i, V3 b, double d, int j, int k, int l, float m) { return a.x + a.y * 2 + a.z * 3 + i * 4 + b.x * 5 + b.y * 6 + b.z * 7 + d * 8 + j * 9 + k * 10 + l * 11 + m * 12; }
float xf_sum(Xf x) { return (float)x.id + x.x + x.y * 2 + x.angle * 3 + (float)x.mode * 4; }
Xf xf_make(uint32_t id, float x, float y, float angle, int mode) { Xf r; r.id = id; r.x = x; r.y = y; r.angle = angle; r.mode = mode; return r; }
int xf_apply(const Xf* sets, int n, float dt) { int s = 0; for (int i = 0; i < n; i++) s += (int)((sets[i].x + sets[i].y + sets[i].angle) * dt) + (int)sets[i].id * sets[i].mode; return s; }
void xf_fill(Xf* out, int n) { for (int i = 0; i < n; i++) { out[i].id = (uint32_t)i; out[i].x = i * 0.5f; out[i].y = -i; out[i].angle = i * 0.25f; out[i].mode = i % 3; } }
void xf_init(Xf* out, int id) { out->id = (uint32_t)id; out->x = 1.5f; out->y = 2.5f; out->angle = 0.5f; out->mode = id * 2; }
double padded_sum(Padded p) { return p.s + (double)p.l + p.f; }
Padded padded_make(int s, int64_t l, float f) { Padded p; p.s = s; p.l = l; p.f = f; return p; }
int tail_sum(Tail t) { return t.a + t.b; }
int bytes_sum(Bytes b) { return b.r + b.g + b.b + b.a; }

// ---- callbacks
int fold_ints(const int32_t* a, int n, int init, binop_fn f) { int acc = init; for (int i = 0; i < n; i++) acc = f(acc, a[i]); return acc; }
double map_sum(const double* a, int n, mapd_fn f) { double s = 0; for (int i = 0; i < n; i++) s += f(a[i]); return s; }
void for_each(int n, visit_fn f) { for (int i = 0; i < n; i++) f(i, i * 0.5); }
int64_t accum(int n, acc_fn f, void* tag) { int64_t acc = 0; for (int i = 0; i < n; i++) acc = f(acc, (float)i, tag); return acc; }
void sort_ints(int32_t* a, int n, cmp_fn cmp) { for (int i = 1; i < n; i++) for (int j = i; j > 0 && cmp(&a[j - 1], &a[j]) > 0; j--) { int32_t t = a[j]; a[j] = a[j - 1]; a[j - 1] = t; } }
int call_twice(binop_fn a, binop_fn b, int x, int y) { return a(x, y) * 1000 + b(x, y); }
int call_null(binop_fn f) { return f ? f(3, 4) : -1; }
int many(int count, binop_fn f) { int s = 0; for (int i = 0; i < count; i++) s += f(i, s & 7); return s; }
double wide_cb(int a, int b, int c, int d, int e, double f, binop_fn fn, int g) { return fn(a + b, c + d) * 1000.0 + e + f + g; }
