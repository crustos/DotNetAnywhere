// Control-flow shapes for the wasm JIT (native/src/WasmJIT.c), which emits structured code (wasm loops, blocks and ifs) when a method's graph is
// reducible, and the dispatch loop otherwise. Every method here is pure integer code with a loop or a branch; the output must equal Mono's. With
// DNA_WASM_JIT_SLICE=1 each of the outermost loops gives up the processor on every pass, so the code is also left and resumed in the middle.
using System;

class WasmJitFlow {
    static int Nested(int n) {
        int t = 0;
        for (int i = 0; i < n; i++) {
            if (i % 7 == 3) continue;
            for (int j = 0; j < n; j++) {
                if (j > i + 4) break;
                if ((i + j) % 5 == 0) continue;
                t += i * j - (i ^ j);
            }
            if (t > 100000) break;
        }
        return t;
    }
    static int Labeled(int n) {                     // goto out of nested loops
        int found = -1;
        for (int i = 1; i < n; i++)
            for (int j = 1; j < n; j++)
                if (i * j == 91) { found = i * 1000 + j; goto done; }
        done:
        return found;
    }
    static int WhileTrue(int n) { int i = 0, s = 0; while (true) { i++; if (i > n) break; if ((i & 3) == 0) continue; s += i; } return s; }
    static int DoWhile(int n) { int i = 0, s = 0; do { s += i * i; i += 3; } while (i < n); return s; }
    static int ReturnInLoop(int n, int target) {
        for (int i = 0; i < n; i++) for (int j = 0; j < n; j++) if (i * n + j == target) return i * 100 + j;
        return -1;
    }
    static int LoopInBranches(int n, int mode) {      // loops inside the arms of an if, with code after
        int s = 0;
        if (mode == 0) { for (int i = 0; i < n; i++) s += i; }
        else if (mode == 1) { int k = n; while (k > 0) { s += k; k -= 2; } }
        else { for (int i = 0; i < n; i++) for (int j = 0; j < i; j++) s += j; }
        s *= 3;
        if (mode != 2) { for (int i = 0; i < 5; i++) s ^= i; }
        return s;
    }
    static int Sequential(int n) {                    // loops one after another, each using what the one before made
        int[] a = new int[n + 1];
        for (int i = 0; i <= n; i++) a[i] = i * 3 % 11;
        int s = 0;
        for (int i = 0; i <= n; i++) s += a[i];
        for (int i = n; i > 0; i--) a[i - 1] += a[i];
        for (int i = 0; i <= n; i++) s ^= a[i];
        return s;
    }
    static int Irreducible(int x, int n) {            // two ways into the loop A,B: not reducible, so it needs the dispatch loop
        int c = 0;
        if (x > 0) goto B;
        A: c += 3; if (c > n) return c;
        B: c += 5; if (c < n) goto A;
        return c * 2;
    }
    static int Shortcuts(int n) {                     // && || ?: in a loop
        int s = 0;
        for (int i = 0; i < n; i++) {
            if ((i % 3 == 0 && i % 5 != 0) || i % 7 == 6) s += i; else s -= (i & 1) == 0 ? 1 : 2;
            s += i > 10 ? (i > 20 ? 3 : 2) : 1;
        }
        return s;
    }
    static int Chain(int x) { if (x < 0) return -1; if (x == 0) return 0; if (x < 10) return x * 2; if (x < 100) return x * 3; if (x < 1000) return x * 4; return x; }
    static int Countdown(int n) { int c = 0; while (n != 1) { n = (n & 1) == 0 ? n >> 1 : 3 * n + 1; c++; if (c > 1000) break; } return c; }
    static int Faulting(int[] a, int n) { int s = 0; for (int i = 0; i < n; i++) { s += a[i]; a[i] = s; } return s; }
    static long Triple(int n) {                       // three levels
        long t = 0;
        for (int i = 0; i < n; i++) for (int j = 0; j < n; j++) { if (j == i) continue; for (int k = 0; k < n; k++) { if (k > j + i) break; t += (i + 1) * (j + 2) * (k + 3); } }
        return t;
    }
    static int Swap(int n) { int a = 1, b = 2, c = 3; for (int i = 0; i < n; i++) { int t = a; a = b; b = c; c = t + a; if (c > 1000) c -= 1000; } return a * 10000 + b * 100 + c; }

    static string Try(Func<int> f) { try { return f().ToString(); } catch (IndexOutOfRangeException) { return "IOOR"; } catch (NullReferenceException) { return "NRE"; } }

    static void Main() {
        for (int n = 0; n < 40; n += 7) Console.WriteLine("nested " + Nested(n) + " " + Labeled(n * 3) + " " + WhileTrue(n) + " " + DoWhile(n) + " " + Swap(n));
        for (int t = -1; t < 60; t += 13) Console.WriteLine("return " + ReturnInLoop(8, t) + " " + ReturnInLoop(0, t));
        for (int m = 0; m < 3; m++) for (int n = 0; n < 12; n += 5) Console.WriteLine("branches " + LoopInBranches(n, m));
        for (int n = 0; n < 50; n += 17) Console.WriteLine("sequential " + Sequential(n) + " " + Shortcuts(n) + " " + Triple(n % 9));
        for (int x = -1; x < 3; x++) for (int n = 0; n < 40; n += 9) Console.WriteLine("irreducible " + Irreducible(x, n));
        for (int x = -5; x < 2000; x += 337) Console.WriteLine("chain " + Chain(x) + " " + Countdown(x < 1 ? 1 : x));
        int[] arr = new int[10]; for (int i = 0; i < 10; i++) arr[i] = i;
        Console.WriteLine("faulting " + Try(() => Faulting(arr, 10)) + " " + Try(() => Faulting(arr, 11)) + " " + Try(() => Faulting(null, 3)) + " " + arr[0] + " " + arr[5] + " " + arr[9]);
        Console.WriteLine("big " + Nested(300) + " " + Sequential(2000) + " " + Triple(30) + " " + Countdown(837799) + " " + DoWhile(100000));
    }
}
