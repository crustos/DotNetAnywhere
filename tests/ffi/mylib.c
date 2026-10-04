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
