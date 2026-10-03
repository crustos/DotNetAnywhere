using System;

// Exceptions thrown more than one frame below the catch. DNA used to reload the thread's
// current frame from a stale pointer after unwinding, so anything deeper than one frame
// ran off the end of an opcode stream (and the GC then scanned dead frames).
class Node { public int V; public Node Next; }

class Program {
    static int log;                                   // digits appended in execution order
    static void Note(int d) { log = log * 10 + d; }

    static void Thrower() { throw new ArgumentOutOfRangeException("x"); }
    static void Mid1() { Thrower(); }
    static int Mid2(int a, long b, double c) { Mid1(); return 1; }

    static void WithFinally(int tag) {
        try { Mid1(); }
        finally { Note(tag); }
    }
    static void TwoFinallys() {
        try { WithFinally(1); }
        finally { Note(2); }
    }
    static void Rethrower() {
        try { Mid1(); }
        catch (ArgumentOutOfRangeException) { Note(4); throw; }
    }
    static Node Build(int n) {
        Node head = null;
        for (int i = 0; i < n; i++) { Node x = new Node(); x.V = i; x.Next = head; head = x; }
        return head;
    }

    public static int Main() {
        // 1. a catch several frames up, mixed parameter types
        bool caught = false;
        try { Mid2(1, 2L, 3.0); } catch (ArgumentOutOfRangeException) { caught = true; }
        if (!caught) return 1;

        // 2. finally blocks run innermost first while unwinding, then the catch
        log = 0;
        try { TwoFinallys(); } catch (ArgumentOutOfRangeException) { Note(3); }
        if (log != 123) return 2;

        // 3. rethrow crosses frames too
        log = 0;
        try { Rethrower(); } catch (ArgumentException) { Note(5); }
        if (log != 45) return 3;

        // 4. execution continues correctly after the catch, with locals intact
        int before = 42;
        string s = "kept";
        try { Mid2(0, 0, 0); } catch (ArgumentOutOfRangeException) { }
        if (before != 42 || s != "kept") return 4;

        // 5. many unwinds in a loop leave the frame chain consistent
        int count = 0;
        for (int i = 0; i < 200; i++) {
            try { Mid2(i, i, i); } catch (ArgumentOutOfRangeException) { count++; }
        }
        if (count != 200) return 5;

        // 6. the GC after a caught exception still finds live objects
        Node keep = Build(300);
        try { Mid2(1, 1, 1); } catch (ArgumentOutOfRangeException) { }
        for (int i = 0; i < 20; i++) { Build(200); GC.Collect(); }
        int sum = 0, n = 0;
        for (Node p = keep; p != null; p = p.Next) { sum += p.V; n++; }
        if (n != 300 || sum != 299 * 300 / 2) return 6;

        // 7. an exception caught in a callee never disturbs the caller
        log = 0;
        try { WithFinally(7); } catch (Exception) { Note(8); }
        if (log != 78) return 7;

        // 8. catch and throw in ONE method, with a finally nested between them: it must run
        //    before the catch handler (it used to be skipped)
        log = 0;
        try { try { throw new InvalidOperationException(); } finally { Note(1); } }
        catch (InvalidOperationException) { Note(2); }
        if (log != 12) return 8;

        // 9. two nested finallys, innermost first, then the catch
        log = 0;
        try { try { try { throw new InvalidOperationException(); } finally { Note(1); } } finally { Note(2); } }
        catch (InvalidOperationException) { Note(3); }
        if (log != 123) return 9;

        // 10. a finally attached to the SAME try as the catch runs after the catch, not before
        log = 0;
        try { throw new InvalidOperationException(); }
        catch (InvalidOperationException) { Note(1); }
        finally { Note(2); }
        if (log != 12) return 10;

        // 11. an inner try/catch that doesn't match: its finally runs, then the outer catch
        log = 0;
        try {
            try { throw new InvalidOperationException(); }
            catch (ArgumentException) { Note(9); }
            finally { Note(1); }
        } catch (InvalidOperationException) { Note(2); }
        if (log != 12) return 11;

        // 12. after those unwinds, ordinary try/finally (no exception) still behaves normally
        log = 0;
        try { Note(1); } finally { Note(2); }
        Note(3);
        try { try { Note(4); } finally { Note(5); } } finally { Note(6); }
        if (log != 123456) return 12;

        // 13. and an exception thrown *after* all of that is still caught correctly
        log = 0;
        try { try { throw new InvalidOperationException(); } finally { Note(1); } }
        catch (InvalidOperationException) { Note(2); }
        if (log != 12) return 13;
        return 0;
    }
}
