using System;

// try / catch / finally regions in loops and nested, compared with Mono line for line. Three bugs lived here:
//  1. `leave` picked the finally clause to run from a stale instruction position (only saved at calls), so a try
//     body containing no call never ran its finally block;
//  2. a catch handler compiled before its exception type had been used had the exception reference left on the
//     evaluation stack (its `pop` was sized from an unfilled type), which overflowed onto the parameters. That
//     depends on the order things are first used, so every shape below has its own fresh exception class;
//  3. `leave` ran only the innermost finally block, so `return` from nested try/finally blocks skipped the rest.
class Ex1 : Exception { public Ex1() : base("x") { } }
class Ex2 : Exception { public Ex2() : base("x") { } }
class Ex3 : Exception { public Ex3() : base("x") { } }
class Ex4 : Exception { public Ex4() : base("x") { } }
class Ex5 : Exception { public Ex5() : base("x") { } }
class Ex6 : Exception { public Ex6() : base("x") { } }
class Ex7 : Exception { public Ex7() : base("x") { } }
class Ex8 : Exception { public Ex8() : base("x") { } }

public class Program {
    static string log = "";
    static int K(int x) { return x; }
    static void Throw1(int d) { if (d == 0) throw new Ex1(); Throw1(d - 1); }
    static void Throw2(int d) { if (d == 0) throw new Ex2(); Throw2(d - 1); }
    static void Throw3(int d) { if (d == 0) throw new Ex3(); Throw3(d - 1); }
    static void Throw4(int d) { if (d == 0) throw new Ex4(); Throw4(d - 1); }
    static void Throw5(int d) { if (d == 0) throw new Ex5(); Throw5(d - 1); }
    static void Throw6(int d) { if (d == 0) throw new Ex6(); Throw6(d - 1); }
    static void Throw7(int d) { if (d == 0) throw new Ex7(); Throw7(d - 1); }
    static void Throw8(int d) { if (d == 0) throw new Ex8(); Throw8(d - 1); }

    // ---- the loop shapes
    static int CatchOnlyDeep(int n)    { int c = 0; for (int i = 0; i < n; i++) { try { Throw1(2); } catch (Ex1) { c += 1; } } return c; }
    static int CatchOnlyShallow(int n) { int c = 0; for (int i = 0; i < n; i++) { try { Throw2(0); } catch (Ex2) { c += 1; } } return c; }
    static int CatchFinallyDeep(int n) { int c = 0; for (int i = 0; i < n; i++) { try { Throw3(2); } catch (Ex3) { c += 1; } finally { c += 2; } } return c; }
    static int CatchWhile(int n)       { int c = 0; int i = 0; while (i < n) { try { Throw4(1); } catch (Ex4) { c += 1; } i++; } return c; }
    static int CatchBase(int n)        { int c = 0; for (int i = 0; i < n; i++) { try { Throw5(3); } catch (Exception) { c += 1; } } return c; }
    static int CatchSameFrame(int n)   { int c = 0; for (int i = 0; i < n; i++) { try { throw new Ex6(); } catch (Ex6) { c += 1; } finally { c += 2; } } return c; }
    static int FinallyInside(int n)    { int c = 0; for (int i = 0; i < n; i++) { try { try { Throw7(2); } finally { c += 2; } } catch (Ex7) { c += 1; } } return c; }
    static int AfterTheTry(int n)      { int c = 0; for (int i = 0; i < n; i++) { try { Throw8(2); } catch (Ex8) { c += 1; } finally { c += 2; } c += 10; } return c; }

    // ---- finally without any exception, with and without a call in the try
    static int FinallyNoCall(int n)    { int c = 0; for (int i = 0; i < n; i++) { try { c += 1; } finally { c += 2; } } return c; }
    static int FinallyWithCall(int n)  { int c = 0; for (int i = 0; i < n; i++) { try { c += K(1); } finally { c += 2; } } return c; }
    static int FinallyCallBefore(int n){ int c = 0; for (int i = 0; i < n; i++) { K(0); try { c += 1; } finally { c += 2; } } return c; }
    static int FinallyNoLoop()         { int c = 0; try { c += 1; } finally { c += 2; } return c; }
    static int CatchFinallyNoThrow(int n) { int c = 0; for (int i = 0; i < n; i++) { try { c += 1; } catch (Ex1) { c += 5; } finally { c += 2; } } return c; }

    // ---- leaving through several finally blocks
    static int Nested()  { try { try { log += "a"; return 1; } finally { log += "b"; } } finally { log += "c"; } }
    static int Triple()  { try { try { try { log += "1"; return 7; } finally { log += "2"; } } finally { log += "3"; } } finally { log += "4"; } }
    static void BreakThroughTwo() { for (int i = 0; i < 3; i++) { try { try { if (i == 1) break; log += "x"; } finally { log += "y"; } } finally { log += "z"; } } }
    static int CatchReturnFinally() { try { try { log += "p"; throw new Ex1(); } catch (Ex1) { log += "q"; return 5; } finally { log += "r"; } } finally { log += "s"; } }
    static void ContinueThroughTwo() { for (int i = 0; i < 3; i++) { try { try { if (i == 1) continue; log += "m"; } finally { log += "n"; } } finally { log += "o"; } } }

    public static void Main() {
        Console.WriteLine("catch only, deep throw       " + CatchOnlyDeep(5) + " (5)");
        Console.WriteLine("catch only, shallow throw    " + CatchOnlyShallow(5) + " (5)");
        Console.WriteLine("catch+finally, deep throw    " + CatchFinallyDeep(5) + " (15)");
        Console.WriteLine("catch in a while loop        " + CatchWhile(5) + " (5)");
        Console.WriteLine("catch (Exception)            " + CatchBase(5) + " (5)");
        Console.WriteLine("catch+finally, same frame    " + CatchSameFrame(5) + " (15)");
        Console.WriteLine("finally inside, catch outside " + FinallyInside(5) + " (15)");
        Console.WriteLine("code after the try           " + AfterTheTry(5) + " (65)");
        Console.WriteLine("finally, no call in the try  " + FinallyNoCall(5) + " (15)");
        Console.WriteLine("finally, a call in the try   " + FinallyWithCall(5) + " (15)");
        Console.WriteLine("finally, call before the try " + FinallyCallBefore(5) + " (15)");
        Console.WriteLine("finally, no loop             " + FinallyNoLoop() + " (3)");
        Console.WriteLine("catch+finally, nothing thrown " + CatchFinallyNoThrow(5) + " (15)");
        log = ""; int r = Nested();            Console.WriteLine("nested return                " + log + " r=" + r + " (abc r=1)");
        log = ""; r = Triple();                Console.WriteLine("triple nested return         " + log + " r=" + r + " (1234 r=7)");
        log = ""; BreakThroughTwo();           Console.WriteLine("break through two            " + log + " (xyzyz)");
        log = ""; r = CatchReturnFinally();    Console.WriteLine("catch, return, finally       " + log + " r=" + r + " (pqrs r=5)");
        log = ""; ContinueThroughTwo();        Console.WriteLine("continue through two         " + log + " (mnonomno)");
    }
}
