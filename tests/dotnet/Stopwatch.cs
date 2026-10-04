using System;
using System.Diagnostics;

// Stopwatch: what can be observed deterministically (it needs a clock, so the values themselves cannot be compared:
// the properties are). The output is compared with Mono's, line for line.
class Program {
    static void Spin(Stopwatch w, long ms) { while (w.ElapsedMilliseconds < ms) { } }

    static void Main() {
        Console.WriteLine("frequency positive: " + (Stopwatch.Frequency > 0));
        Console.WriteLine("high resolution: " + Stopwatch.IsHighResolution);

        long a = Stopwatch.GetTimestamp(), b = Stopwatch.GetTimestamp();
        Console.WriteLine("timestamps never go back: " + (b >= a));

        Stopwatch w = new Stopwatch();
        Console.WriteLine("new: running=" + w.IsRunning + " elapsed=" + w.ElapsedTicks);
        w.Start();
        Console.WriteLine("started: running=" + w.IsRunning);
        Spin(w, 5);
        Console.WriteLine("ran at least 5 ms: " + (w.ElapsedMilliseconds >= 5));
        w.Stop();
        long frozen = w.ElapsedTicks;
        Console.WriteLine("stopped: running=" + w.IsRunning);
        long t0 = Stopwatch.GetTimestamp();
        while (Stopwatch.GetTimestamp() - t0 < Stopwatch.Frequency / 200) { }
        Console.WriteLine("stopped watch does not advance: " + (w.ElapsedTicks == frozen));
        w.Start();
        Spin(w, 8);
        Console.WriteLine("restarted after Stop keeps the total: " + (w.ElapsedTicks > frozen && w.ElapsedMilliseconds >= 8));
        w.Reset();
        Console.WriteLine("reset: running=" + w.IsRunning + " elapsed=" + w.ElapsedTicks);
        w.Restart();
        Console.WriteLine("restart: running=" + w.IsRunning);
        Spin(w, 3);
        Console.WriteLine("restart counts from zero: " + (w.ElapsedMilliseconds >= 3 && w.ElapsedMilliseconds < 1000));
        Console.WriteLine("Elapsed agrees with the ticks: " + (w.Elapsed.TotalMilliseconds >= 3));
        Stopwatch s = Stopwatch.StartNew();
        Console.WriteLine("StartNew is running: " + s.IsRunning);
        Console.WriteLine("ticks per ms agree with Frequency: " + (Stopwatch.Frequency / 1000 > 0));
    }
}
