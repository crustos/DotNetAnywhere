// A native program that hosts DotNetAnywhere (native/src/Host.h) and calls the static methods of Managed.cs.
// Each line it prints is also printed by Oracle.cs under Mono.  Lines that start with # are checks of the host API itself.
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "Host.h"

static DNA_Assembly *asm_;
static int failures = 0;

static DNA_Method *find(const char *name, const char *sig) {
	DNA_Method *m = DNA_Find(asm_, "T", "Calc", name, sig);
	if (!m) { printf("#FAIL find %s: %s\n", name, DNA_Error()); failures++; }
	return m;
}

static DNA_Value call(const char *name, const char *sig, const DNA_Value *a, int n) {
	DNA_Value r;
	memset(&r, 0, sizeof r);
	DNA_Method *m = find(name, sig);
	if (m && DNA_Call(m, a, n, &r) != 0) { printf("#FAIL call %s: %s\n", name, DNA_Error()); failures++; }
	return r;
}

static void expect_error(const char *what, int rc, const char *needle) {
	if (rc == 0) { printf("#FAIL %s: no error\n", what); failures++; return; }
	if (strstr(DNA_Error(), needle) == NULL) { printf("#FAIL %s: error was '%s', wanted '%s'\n", what, DNA_Error(), needle); failures++; return; }
	printf("#ok %s\n", what);
}

static void print_ints(const char *label, const int32_t *v, int n) {
	printf("%s: ", label);
	for (int i = 0; i < n; i++) printf(i ? ",%d" : "%d", v[i]);
	printf("\n");
}

int main(int argc, char **argv) {
	const char *dll = argc > 1 ? argv[1] : "Managed.dll";
	DNA_Init();
	asm_ = DNA_Load(dll);
	if (!asm_) { printf("#FAIL load: %s\n", DNA_Error()); return 1; }

	DNA_Value a[5], r;
	a[0] = DNA_Int(40); a[1] = DNA_Int(2);
	printf("AddI: %d\n", call("AddI", "ii>i", a, 2).u.i);
	a[0] = DNA_Int(-7); a[1] = DNA_Int(3);
	printf("AddI neg: %d\n", call("AddI", NULL, a, 2).u.i);
	a[0] = DNA_Long(3000000000LL); a[1] = DNA_Long(7);
	printf("MulL: %lld\n", (long long)call("MulL", "ll>l", a, 2).u.l);
	a[0] = DNA_Float(1.5f); a[1] = DNA_Float(4.0f);
	printf("ScaleF: %d\n", (int)(call("ScaleF", "ff>f", a, 2).u.f * 1000));
	a[0] = DNA_Double(3.0); a[1] = DNA_Double(4.0);
	printf("Hyp: %lld\n", (long long)(call("Hyp", "dd>d", a, 2).u.d * 1000));
	a[0] = DNA_Int(5);
	printf("IsPos true: %d\n", call("IsPos", "i>i", a, 1).u.i);
	a[0] = DNA_Int(-5);
	printf("IsPos false: %d\n", call("IsPos", "i>i", a, 1).u.i);
	a[0] = DNA_Int(0x1234);
	printf("Low: %d\n", call("Low", "i>i", a, 1).u.i);
	a[0] = DNA_Int(70000);
	printf("Narrow: %d\n", call("Narrow", "i>i", a, 1).u.i);
	a[0] = DNA_Int((int32_t)4000000000u); a[1] = DNA_Int(1); a[2] = DNA_Int((int32_t)3000000000u);
	printf("UClamp: %u\n", (uint32_t)call("UClamp", "iii>i", a, 3).u.i);
	a[0] = DNA_Int(1); a[1] = DNA_Double(2.9); a[2] = DNA_Long(3); a[3] = DNA_Float(4.9f); a[4] = DNA_Int(5);
	printf("Mixed: %d\n", call("Mixed", "idlfi>i", a, 5).u.i);
	call("Nop", NULL, NULL, 0);
	printf("Nop: done\n");

	// a static that keeps its value from one call to the next
	for (int i = 1; i <= 3; i++) printf("Next %d: %d\n", i, call("Next", NULL, NULL, 0).u.i);

	// overloads are told apart by the signature
	a[0] = DNA_Int(5);  printf("Over int: %d\n", call("Over", "i>i", a, 1).u.i);
	a[0] = DNA_Long(5); printf("Over long: %d\n", call("Over", "l>i", a, 1).u.i);

	// strings: UTF-8 in, UTF-8 out (the length and the code unit sum are of the UTF-16 string, so the conversion is checked)
	const char *w = "w\xc3\xb6rld \xe2\x9c\x93 \xf0\x9f\x98\x80";
	a[0] = DNA_Str(w);
	r = call("Greet", NULL, a, 1);
	printf("Greet: %s\n", r.u.s);
	printf("Len: %d\n", call("Len", NULL, a, 1).u.i);
	printf("CodeSum: %d\n", call("CodeSum", NULL, a, 1).u.i);
	a[0] = DNA_Str("ab"); a[1] = DNA_Int(5);
	printf("Repeat: %s\n", call("Repeat", NULL, a, 2).u.s);
	a[0] = DNA_Str("0123456789"); a[1] = DNA_Int(300);
	printf("Repeat long: %d\n", (int)strlen(call("Repeat", NULL, a, 2).u.s));
	a[0] = DNA_Str("a"); a[1] = DNA_Str(""); a[2] = DNA_Str("c");
	printf("Join3: %s\n", call("Join3", NULL, a, 3).u.s);

	// arrays in
	int32_t five[] = { 1, 2, 3, 4, 5 };
	a[0] = DNA_Array(five, 5, 4);
	printf("Sum: %d\n", call("Sum", NULL, a, 1).u.i);
	a[0] = DNA_Array(NULL, 0, 4);
	printf("Sum empty: %d\n", call("Sum", NULL, a, 1).u.i);
	int64_t big[] = { 4000000000LL, 5000000000LL, -1 };
	a[0] = DNA_Array(big, 3, 8);
	printf("SumL: %lld\n", (long long)call("SumL", NULL, a, 1).u.l);
	// ... and written to by the callee: seen by the caller
	int32_t f[6] = { 0 };
	a[0] = DNA_Array(f, 6, 4); a[1] = DNA_Int(10);
	call("Fill", NULL, a, 2);
	print_ints("Fill", f, 6);
	double d1[] = { 1, 2, 3 }, d2[] = { 4, 5, 6 };
	a[0] = DNA_Array(d1, 3, 8); a[1] = DNA_Array(d2, 3, 8);
	printf("Dot: %lld\n", (long long)(call("Dot", NULL, a, 2).u.d * 1000));
	char text[] = "hello, World 42";
	a[0] = DNA_Array(text, (int)strlen(text), 1);
	call("Upper", NULL, a, 1);
	printf("Upper: %s\n", text);

	// arrays out
	a[0] = DNA_Int(6);
	r = call("Bytes", NULL, a, 1);
	printf("Bytes: ");
	for (int i = 0; i < r.len; i++) printf(i ? ",%d" : "%d", ((unsigned char*)r.u.data)[i]);
	printf("\n");
	a[0] = DNA_Int(7);
	r = call("Squares", NULL, a, 1);
	print_ints("Squares", (int32_t*)r.u.data, r.len);
	a[0] = DNA_Int(5);
	r = call("Halves", NULL, a, 1);
	printf("Halves: ");
	for (int i = 0; i < r.len; i++) printf(i ? ",%d" : "%d", (int)(((float*)r.u.data)[i] * 10));
	printf("\n");

	// what was passed in survives a collection while the managed code runs
	int32_t three[] = { 1, 2, 3 };
	a[0] = DNA_Str("hello"); a[1] = DNA_Array(three, 3, 4); a[2] = DNA_Int(3000);
	printf("Churn: %d\n", call("Churn", NULL, a, 3).u.i);
	int32_t two[] = { 10, 20 };
	a[0] = DNA_Str("a longer string than before"); a[1] = DNA_Array(two, 2, 4); a[2] = DNA_Int(3000);
	printf("Churn again: %d\n", call("Churn", NULL, a, 3).u.i);

	// objects, by handle
	{
		DNA_Value o[2], r2;
		int64_t h1, h2;
		memset(&r2, 0, sizeof r2);
		a[0] = DNA_Int(10);
		r2 = call("NewCounter", "i>o", a, 1);
		h1 = r2.u.l;
		if (h1 == 0 || r2.kind != 'o') { printf("#FAIL a new object has no handle\n"); failures++; }
		o[0] = DNA_Obj(h1); o[1] = DNA_Int(5);
		printf("Obj bump 1: %d\n", call("Bump", "oi>i", o, 2).u.i);
		o[1] = DNA_Int(7);
		printf("Obj bump 2: %d\n", call("Bump", "oi>i", o, 2).u.i);
		o[0] = DNA_Obj(h1);
		r2 = call("Same", "o>o", o, 1);
		h2 = r2.u.l;
		if (h2 != 0 && h2 != h1) printf("#ok the same object under another handle gets a handle of its own\n");
		else { printf("#FAIL same object, same handle? %lld %lld\n", (long long)h1, (long long)h2); failures++; }
		o[0] = DNA_Obj(h2); o[1] = DNA_Int(1);
		printf("Obj same: %d\n", call("Bump", "oi>i", o, 2).u.i);
		if (DNA_Release(h1) != 0) { printf("#FAIL release: %s\n", DNA_Error()); failures++; }
		o[0] = DNA_Obj(h2); o[1] = DNA_Int(1);
		printf("Obj after drop: %d\n", call("Bump", "oi>i", o, 2).u.i);     // h1 was released, h2 still holds the object
		o[0] = DNA_Obj(h2);
		printf("Obj total: %d\n", call("CTotal", "o>i", o, 1).u.i);
		o[0] = DNA_Obj(h2); o[1] = DNA_Str("x");
		printf("Obj tag 1: %s\n", call("CTag", "os>s", o, 2).u.s);
		o[0] = DNA_Obj(h2); o[1] = DNA_Str("y");
		printf("Obj tag 2: %s\n", call("CTag", "os>s", o, 2).u.s);
		o[0] = DNA_Obj(0);
		printf("Obj null: %d\n", call("IsNull", "o>i", o, 1).u.i);
		o[0] = DNA_Obj(h2);
		printf("Obj not null: %d\n", call("IsNull", "o>i", o, 1).u.i);

		// released handles and wrong kinds are refused, and nothing runs
		DNA_Method *bump = find("Bump", NULL);
		o[0] = DNA_Obj(h1); o[1] = DNA_Int(1);
		expect_error("a released handle", DNA_Call(bump, o, 2, NULL), "is not live");
		o[0] = DNA_Obj(999999); 
		expect_error("a handle never given out", DNA_Call(bump, o, 2, NULL), "is not live");
		o[0] = DNA_Int(5);
		expect_error("an int where an object is wanted", DNA_Call(bump, o, 2, NULL), "parameter 1 is kind 'o'");
		expect_error("releasing twice", DNA_Release(h1), "not live");
		if (DNA_Release(0) == 0) printf("#ok releasing 0 does nothing\n"); else { printf("#FAIL release 0\n"); failures++; }
		DNA_Release(h2);
		if (DNA_LiveHandles() == 0) printf("#ok no handle is left\n"); else { printf("#FAIL %d handles left\n", DNA_LiveHandles()); failures++; }

		// 200 objects held by handles alone, a lot of garbage, then their children are still there
		{
			int64_t many[200];
			long long okHist = 0, okTotal = 0;
			for (int i = 0; i < 200; i++) {
				a[0] = DNA_Int(i);
				many[i] = call("NewCounter", "i>o", a, 1).u.l;
				o[0] = DNA_Obj(many[i]); o[1] = DNA_Int(i & 7);
				call("Bump", "oi>i", o, 2);
			}
			int32_t three2[] = { 1, 2, 3 };
			a[0] = DNA_Str("held"); a[1] = DNA_Array(three2, 3, 4); a[2] = DNA_Int(6000);
			printf("Obj churn: %d\n", call("Churn", NULL, a, 3).u.i);
			for (int i = 0; i < 200; i++) {
				o[0] = DNA_Obj(many[i]); o[1] = DNA_Int(i & 7);
				okHist += call("CHist", "oi>i", o, 2).u.i;
				o[0] = DNA_Obj(many[i]);
				okTotal += call("CTotal", "o>i", o, 1).u.i - (i + (i & 7));
			}
			printf("Obj children: %lld\n", okHist);
			printf("Obj totals: %lld\n", okTotal);
			for (int i = 0; i < 200; i++) DNA_Release(many[i]);
			if (DNA_LiveHandles() == 0) printf("#ok 200 handles released\n"); else { printf("#FAIL leak %d\n", DNA_LiveHandles()); failures++; }
		}
	}

	// null results
	r = call("Nothing", NULL, NULL, 0);
	if (r.u.s == NULL) printf("#ok a null string comes back as NULL\n"); else { printf("#FAIL null string\n"); failures++; }
	r = call("NoInts", NULL, NULL, 0);
	if (r.u.data == NULL && r.len == 0) printf("#ok a null array comes back as NULL\n"); else { printf("#FAIL null array\n"); failures++; }

	// refusals: nothing runs, and the reason says what is wrong
	{
		DNA_Method *m = find("AddI", NULL);
		DNA_Value bad[2];
		bad[0] = DNA_Int(1); bad[1] = DNA_Long(2);
		expect_error("a long where an int is wanted", DNA_Call(m, bad, 2, NULL), "parameter 2 is kind 'i'");
		expect_error("too few arguments", DNA_Call(m, bad, 1, NULL), "takes 2 argument(s), 1 given");
		bad[0] = DNA_Str("x"); bad[1] = DNA_Int(2);
		expect_error("a string where an int is wanted", DNA_Call(m, bad, 2, NULL), "parameter 1 is kind 'i'");
		DNA_Method *s = find("Sum", NULL);
		bad[0] = DNA_Array(five, 5, 8);
		expect_error("an array of the wrong element size", DNA_Call(s, bad, 1, NULL), "4-byte elements in C#, but 8-byte");
		bad[0] = DNA_Array(NULL, 3, 4);
		expect_error("an array of no data", DNA_Call(s, bad, 1, NULL), "no data");
		DNA_Method *c = find("Count", NULL);
		bad[0] = DNA_Array(five, 5, 4);
		expect_error("an array of strings", DNA_Call(c, bad, 1, NULL), "not blittable");
		if (DNA_Find(asm_, "T", "Calc", "Over", NULL) == NULL && strstr(DNA_Error(), "overloaded")) printf("#ok an overloaded name needs a signature\n");
		else { printf("#FAIL overload\n"); failures++; }
		if (DNA_Find(asm_, "T", "Calc", "AddI", "ll>l") == NULL) printf("#ok a wrong signature finds nothing\n");
		else { printf("#FAIL wrong signature\n"); failures++; }
		if (DNA_Find(asm_, "T", "NoSuchClass", "X", NULL) == NULL && strstr(DNA_Error(), "no class")) printf("#ok no such class\n");
		else { printf("#FAIL no such class\n"); failures++; }
		if (DNA_Load("/nonexistent/x.dll") == NULL && strstr(DNA_Error(), "cannot open")) printf("#ok no such file\n");
		else { printf("#FAIL no such file\n"); failures++; }
	}
	// the host is still fine after refused calls
	a[0] = DNA_Int(1); a[1] = DNA_Int(2);
	if (call("AddI", NULL, a, 2).u.i == 3) printf("#ok still working after refusals\n"); else { printf("#FAIL after refusals\n"); failures++; }

	return failures ? 1 : 0;
}
