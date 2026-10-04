using System;
using System.Threading;

// Thread.Sleep in the main thread. When every thread was sleeping or blocked, the scheduler returned instead of waiting,
// and that ended the whole program (rc 1): "after sleep" was never printed. Compared with Mono line for line.
class Flag { public int Set; }

class Program {
    static Flag F = new Flag();
    static void Worker() { Thread.Sleep(10); F.Set = 1; }

    static void Main() {
        Console.WriteLine("before sleep");
        Thread.Sleep(5);
        Console.WriteLine("after sleep");
        new Thread(new ThreadStart(Worker)).Start();
        int waits = 0;
        while (F.Set == 0 && waits < 1000) { Thread.Sleep(2); waits++; }
        Console.WriteLine("the worker ran while the main thread slept: " + (F.Set == 1));
        Console.WriteLine("finished");
    }
}
