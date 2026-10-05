// The C side of tests/host/Nested.cs: functions that managed code calls with [DllImport] (the --ffi build, nested.json), and that call
// managed code again through the host API.  That is a *nested* call: the runtime is already running the managed program.
#include <string.h>
#include "Host.h"

static DNA_Method *nested_find(const char *cls, const char *name, const char *sig) {
	static DNA_Assembly *a;
	if (a == NULL) a = DNA_Loaded("Nested");
	return a ? DNA_Find(a, "", cls, name, sig) : NULL;
}
static DNA_Method *nested_calc(const char *name, const char *sig) {
	static DNA_Assembly *a;
	if (a == NULL) a = DNA_Loaded("Nested");
	return a ? DNA_Find(a, "T", "Calc", name, sig) : NULL;
}

int native_twice(int x) {
	DNA_Value a[2] = { DNA_Int(x), DNA_Int(x) }, r;
	if (DNA_Call(nested_calc("AddI", "ii>i"), a, 2, &r) != 0) return -1;
	return r.u.i;
}

int native_sumsq(int n) {
	DNA_Value a = DNA_Int(n), r;
	int total = 0;
	if (DNA_Call(nested_calc("Squares", "i>b"), &a, 1, &r) != 0) return -1;
	for (int i = 0; i < r.len; i++) total += ((int*)r.u.data)[i];
	return total;
}

int native_greet_len(const char *s) {
	DNA_Value a = DNA_Str(s), r;
	if (DNA_Call(nested_calc("Greet", "s>s"), &a, 1, &r) != 0) return -1;
	return (int)strlen(r.u.s);
}

int native_depth(int n) {
	DNA_Value a, r;
	if (n <= 0) return 0;
	a = DNA_Int(n - 1);
	if (DNA_Call(nested_find("NestedMain", "Recur", "i>i"), &a, 1, &r) != 0) return -1;
	return 1 + r.u.i;
}

// `a` is the managed array itself (zero copy); the managed method writes to a copy of it, which comes back into it
void native_fill(int *a, int n, int v) {
	DNA_Value args[2] = { DNA_Array(a, n, 4), DNA_Int(v) };
	DNA_Call(nested_calc("Fill", "bi>v"), args, 2, NULL);
}

int native_churn(int rounds) {
	int three[3] = { 1, 2, 3 };
	DNA_Value a[3] = { DNA_Str("hello"), DNA_Array(three, 3, 4), DNA_Int(rounds) }, r;
	if (DNA_Call(nested_calc("Churn", "sbi>i"), a, 3, &r) != 0) return -1;
	return r.u.i;
}
