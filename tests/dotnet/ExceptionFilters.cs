using System;
using System.Collections.Generic;

// Exception filters: `catch (T) when (condition)`. The filters of every frame are evaluated
// before any finally block runs (a first pass finds the handler, a second unwinds to it), a filter
// can call code that throws and catches (even using filters of its own). (A filter that itself
// throws is not here: that is swallowed per the CLR spec, but Mono does not do it, so it has its
// own DNA-only test, ExceptionFilterThrows.dnaonly.cs.) DNA ran none of this: a filter clause was never entered and the
// method crashed. The output is compared with Mono's, line for line.
class Log { public static string S = ""; public static void Add(string x) { S += x + " "; } public static void Flush(string label) { Console.WriteLine(label + ": " + S); S = ""; } }

class MyEx : Exception { public int Code; public MyEx(int c) { Code = c; } }

class Program {
    static bool Yes(string tag) { Log.Add("f" + tag); return true; }
    static bool No(string tag) { Log.Add("f" + tag); return false; }
        static void Thrower(int code) { throw new MyEx(code); }

    // a filter that only looks at the exception
    static void ByCode(int code) {
        try { Thrower(code); }
        catch (MyEx e) when (e.Code == 1) { Log.Add("one"); }
        catch (MyEx e) when (e.Code == 2) { Log.Add("two"); }
        catch (MyEx e) { Log.Add("other" + e.Code); }
    }

    // finally blocks must not run until the handler has been chosen
    static void Inner() {
        try { Thrower(5); }
        finally { Log.Add("finally-inner"); }
    }
    static void OrderOfFilters() {
        try {
            try { Inner(); }
            catch (MyEx) when (No("A")) { Log.Add("A"); }
            finally { Log.Add("finally-mid"); }
        }
        catch (MyEx) when (Yes("B")) { Log.Add("B"); }
    }

    // a filter rejects, the exception passes outward, finally blocks run on the way
    static void Passes() {
        try {
            try { Thrower(7); }
            catch (MyEx) when (No("X")) { Log.Add("X"); }
            finally { Log.Add("fin1"); }
        }
        catch (MyEx e) { Log.Add("outer" + e.Code); }
    }

    // a filter sees the frame's own locals and arguments
    static void SeesLocals(int arg) {
        int local = 10;
        string text = "t";
        try { Thrower(arg); }
        catch (MyEx e) when (e.Code + local == 13 && text == "t") { Log.Add("matched" + e.Code); }
        catch (MyEx e) { Log.Add("fell" + e.Code); }
    }

    // a filter that calls a method which throws and catches internally, even with a filter of its own
    static bool Nested(int depth) {
        try { if (depth > 0) throw new MyEx(depth); return false; }
        catch (MyEx e) when (e.Code == 3 && Yes("n" + depth)) { Log.Add("inner-caught"); return true; }
        catch (MyEx) when (No("m" + depth)) { return false; }
        catch (MyEx) { return depth == 1; }
    }
    static void FilterCallsCatcher() {
        try { Thrower(9); }
        catch (MyEx) when (Nested(3) && Nested(1)) { Log.Add("outer-yes"); }
        catch (MyEx) { Log.Add("outer-no"); }
    }

    // a filter's own exception (from a callee) that something inside the filter's call chain handles
    static bool Safe() { try { throw new InvalidOperationException(); } catch (InvalidOperationException) { return true; } }
    static void FilterWithInternalCatch() {
        try { Thrower(4); }
        catch (MyEx e) when (Safe() && e.Code == 4) { Log.Add("safe" + e.Code); }
    }

    // exceptions from a filter nested inside a filter
    static bool Rejects() { try { Thrower(2); } catch (MyEx) when (No("r")) { return false; } return true; }
    static void FilterOfFilter() {
        try { Thrower(8); }
        catch (MyEx) when (RejectsThenFalse()) { Log.Add("no"); }
        catch (MyEx) { Log.Add("fell"); }
    }
    static bool RejectsThenFalse() { try { return Rejects(); } catch (MyEx) { return false; } }

    // rethrow from a filtered handler, then catch again in a caller
    static void Rethrower() {
        try { Thrower(6); }
        catch (MyEx) when (Yes("R")) { Log.Add("rethrow"); throw; }
    }
    static void CatchRethrow() {
        try { Rethrower(); }
        catch (MyEx e) when (e.Code == 6) { Log.Add("caught-again"); }
    }

    // filter on a base type, derived exception; unrelated type never reaches the filter
    static void Hierarchy() {
        try { throw new ArgumentException("a"); }
        catch (InvalidOperationException) when (Yes("ioe")) { Log.Add("ioe"); }
        catch (ArgumentException) when (Yes("arg")) { Log.Add("arg"); }
    }

    // many throws through filters, with allocation, so collections happen while filters are pending
    static int Churn() {
        int caught = 0;
        for (int i = 0; i < 300; i++) {
            List<string> junk = new List<string>();
            try {
                try { junk.Add("x" + i); Thrower(i); }
                catch (MyEx e) when (Allocates(e.Code)) { caught++; }
                finally { junk.Add("done"); }
            }
            catch (MyEx) { caught += 1000; }
        }
        return caught;
    }
    static bool Allocates(int code) {
        string s = "";
        for (int i = 0; i < 20; i++) { s += "pad" + i; }
        return (code % 2) == 0 && s.Length > 0;
    }

    // the same exception object, filtered then handled
    static void SameObject() {
        MyEx thrown = new MyEx(11);
        try { throw thrown; }
        catch (MyEx e) when (object.ReferenceEquals(e, thrown)) { Log.Add("same"); }
    }

    // filter in a loop with continue/break around it
    static void InLoop() {
        for (int i = 0; i < 4; i++) {
            try { Thrower(i); }
            catch (MyEx e) when (e.Code % 2 == 0) { Log.Add("even" + e.Code); continue; }
            catch (MyEx e) { Log.Add("odd" + e.Code); }
            Log.Add("after" + i);
        }
    }

    // A collection while a filter is pending. The frames above the filter's own are suspended, not
    // dead: they hold objects that nothing else references, and their finally blocks still need
    // them. The filter collects and then allocates the same kinds of objects, so anything freed
    // wrongly is reused and overwritten before the finally runs.
    static bool Collecting() {
        GC.Collect();
        List<int>[] junk = new List<int>[40];
        for (int i = 0; i < junk.Length; i++) { junk[i] = new List<int>(); junk[i].Add(-7); junk[i].Add(-7); junk[i].Add(-7); }
        string s = "";
        for (int i = 0; i < 30; i++) { s += "zz" + i; }
        GC.Collect();
        return true;
    }
    static void Deep() {
        List<int> data = new List<int>(); data.Add(1); data.Add(2); data.Add(3);
        string tag = "deep" + 42;
        object[] boxes = new object[3]; boxes[0] = tag; boxes[1] = data;
        try { Thrower(1); }
        finally { Log.Add("fin:" + data.Count + ":" + data[0] + data[1] + data[2] + ":" + tag + ":" + (boxes[1] == data)); }
    }
    static void GcPending() {
        try { Deep(); }
        catch (MyEx) when (Collecting()) { Log.Add("handled"); }
    }

    static void Main() {
        ByCode(1); ByCode(2); ByCode(3); Log.Flush("by code");
        OrderOfFilters(); Log.Flush("order");
        Passes(); Log.Flush("passes");
        SeesLocals(3); SeesLocals(4); Log.Flush("locals");
        FilterCallsCatcher(); Log.Flush("filter calls catcher");
        FilterWithInternalCatch(); Log.Flush("internal catch");
        FilterOfFilter(); Log.Flush("filter of filter");
        CatchRethrow(); Log.Flush("rethrow");
        Hierarchy(); Log.Flush("hierarchy");
        SameObject(); Log.Flush("same object");
        InLoop(); Log.Flush("loop");
        GcPending(); Log.Flush("gc pending");
        Console.WriteLine("churn: " + Churn());
        // an exception that no filter accepts and nothing else catches still reaches the outer handler
        try { try { Thrower(1); } catch (MyEx) when (No("z")) { } }
        catch (MyEx e) { Console.WriteLine("passed out: " + e.Code + " " + Log.S); Log.S = ""; }
    }
}
