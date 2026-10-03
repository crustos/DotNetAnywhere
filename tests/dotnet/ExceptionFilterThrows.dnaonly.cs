using System;

// DNA only (Mono does not implement this part of the spec, so there is nothing to compare with).
// An exception thrown from inside an exception filter, directly or by a method it calls, is
// swallowed: the filter counts as having answered "no", and the search for a handler goes on.
// Self-checking: returns 0 on success, otherwise the number of the failing check.
class MyEx : Exception { public int Code; public MyEx(int c) { Code = c; } }

class Program {
    static string log = "";
    static bool Boom() { log += "boom "; throw new InvalidOperationException("from filter"); }
    static bool CallsBoom() { log += "calls "; return Boom(); }
    static bool Cleanup() { try { throw new InvalidOperationException(); } finally { log += "cleanup "; } }
    static void Thrower(int c) { throw new MyEx(c); }

    public static int Main() {
        // the filter throws; the next clause handles the original exception
        log = "";
        try { try { Thrower(1); } catch (MyEx) when (Boom()) { log += "never "; } catch (MyEx e) { log += "second" + e.Code + " "; } }
        catch (Exception) { return 1; }
        if (log != "boom second1 ") return 2;

        // thrown from a method the filter calls, two frames down
        log = "";
        try { try { Thrower(2); } catch (MyEx) when (CallsBoom()) { log += "never "; } catch (MyEx e) { log += "second" + e.Code + " "; } }
        catch (Exception) { return 3; }
        if (log != "calls boom second2 ") return 4;

        // a finally block in the filter's own call chain still runs when its exception is swallowed
        log = "";
        try { try { Thrower(3); } catch (MyEx) when (Cleanup()) { log += "never "; } catch (MyEx e) { log += "second" + e.Code + " "; } }
        catch (Exception) { return 5; }
        if (log != "cleanup second3 ") return 6;

        // with nothing else to catch it, the ORIGINAL exception propagates (not the one from the filter)
        log = "";
        int code = 0;
        try { try { Thrower(4); } catch (MyEx) when (Boom()) { log += "never "; } }
        catch (MyEx e) { code = e.Code; }
        catch (InvalidOperationException) { return 7; }
        if (code != 4 || log != "boom ") return 8;

        // and it works repeatedly, with the state fully restored each time
        int swallowed = 0;
        for (int i = 0; i < 200; i++) {
            try { try { Thrower(i); } catch (MyEx) when (Boom()) { return 9; } catch (MyEx e) { if (e.Code == i) swallowed++; } }
            catch (Exception) { return 10; }
        }
        if (swallowed != 200) return 11;
        return 0;
    }
}
