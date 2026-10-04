#!/usr/bin/env python3
"""Small benchmarks of DNA against Mono, with a chart.

Each benchmark is a short C# program (the sources are the triple-quoted strings below). The same source is
compiled twice with mcs, once for Mono and once for DNA (against build/corlib.dll), and run on both. A program times
its own work with System.Diagnostics.Stopwatch, after a warm-up run so that Mono's JIT compile time is not charged to
the steady-state benchmarks, and prints a checksum, so every run also checks that DNA and Mono computed the same
answer. Two benchmarks deliberately include start-up and JIT cost: that is where an interpreter has an advantage.

The workloads are sized to take well under a second on DNA, so the whole thing runs in a few seconds (the first run
also compiles the programs, in parallel; they are cached by source hash after that).

    python3 tools/benchmark_mono.py                  # run everything, print a table, write build/benchmark_mono.png
    python3 tools/benchmark_mono.py --repeat 5       # best of 5 (default 3)
    python3 tools/benchmark_mono.py --only int_loop,exceptions
    python3 tools/benchmark_mono.py --list
    python3 tools/benchmark_mono.py --dna build/dna32        # benchmark another binary
    python3 tools/benchmark_mono.py --mono-interp    # add Mono's own interpreter (mono --interpreter) as a third bar
    python3 tools/benchmark_mono.py --json out.json  # save the numbers;   --plot-only out.json  redraws from them
    python3 tools/benchmark_mono.py --show           # open the chart in a window instead of just saving it

Needs: mono and mcs, a built runtime and corlib (`make`), and matplotlib for the chart (the table prints without it).
"""
import argparse
import glob
import concurrent.futures as cf
import datetime
import hashlib
import json
import os
import shutil
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "build")
WORK = os.path.join(BUILD, "benchmarks")

# ---------------------------------------------------------------------------------------------------------------
# The C# programs. Each body defines `const int N` (the size of the work) and `static long Run(int n)`, which does the
# work and returns a checksum. MAIN wraps it: a warm-up call with a small n, then the timed call with N.
# ---------------------------------------------------------------------------------------------------------------

MAIN = """
using System;
using System.Diagnostics;
using System.Collections.Generic;
using System.Text;
using System.Runtime.InteropServices;

public class Program {
/*BODY*/
    public static int Main() {
        /*WARM*/
        Stopwatch sw = Stopwatch.StartNew();
        long result = Run(N);
        sw.Stop();
        Console.WriteLine("SUM " + result);
        Console.WriteLine("US " + (sw.ElapsedTicks / (Stopwatch.Frequency / 1000000L)));
        return 0;
    }
}
"""
WARMUP = "Run(N / 20 + 1);"

STARTUP = """
public class Program { public static int Main() { return 0; } }
"""

# float32 vector math, the staple of game code. VEC_INLINE is one physics step done inline (no calls, long straight-line
# runs of float32 arithmetic on locals); VEC_CALLS calls small vector methods, so it also pays for the calls.
VEC_INLINE = """
    const int N = 120000;
    static long Run(int n) {
        float px = 0.5f, py = 1.5f, pz = -0.5f, vx = 0.1f, vy = 0.2f, vz = -0.1f;
        float ax = 0.3f, ay = -0.2f, az = 0.1f, dt = 0.001f, damp = 0.9999f;
        float nx = 0.6f, ny = 0.8f, nz = 0.0f, acc = 0f;
        for (int i = 0; i < n; i++) {
            vx = (vx + ax * dt) * damp; vy = (vy + ay * dt) * damp; vz = (vz + az * dt) * damp;
            px = px + vx * dt; py = py + vy * dt; pz = pz + vz * dt;
            float d = px * nx + py * ny + pz * nz;
            float t = d * 0.5f + 0.25f;
            float lx = px + (nx - px) * t; float ly = py + (ny - py) * t; float lz = pz + (nz - pz) * t;
            acc = acc + (lx * lx + ly * ly + lz * lz) * 0.0001f;
        }
        return (long)(acc * 65536f) + (long)(px * 1000f);
    }
"""

VEC_CALLS = """
    const int N = 100000;
    static float Dot3(float ax, float ay, float az, float bx, float by, float bz) { return ax * bx + ay * by + az * bz; }
    static float Lerp(float a, float b, float t) { return a + (b - a) * t; }
    static float Quat(float qx, float qy, float qz, float qw, float vx, float vy, float vz) {
        float tx = 2f * (qy * vz - qz * vy);
        float ty = 2f * (qz * vx - qx * vz);
        float tz = 2f * (qx * vy - qy * vx);
        return vx + qw * tx + (qy * tz - qz * ty);
    }
    static long Run(int n) {
        float acc = 0f, t = 0.1f;
        for (int i = 0; i < n; i++) {
            float a = Dot3(t, 1f, 2f, 3f, t, 0.5f);
            float b = Lerp(a, t, 0.25f);
            float c = Quat(0.1f, 0.2f, 0.3f, 0.9f, t, 1f, 0.5f);
            acc = acc + b * 0.001f + c * 0.0001f;
            t = t + 0.0001f;
        }
        return (long)(acc * 65536f);
    }
"""

# Vector3 as a struct held in locals (every field access is ldloca + ldfld/stfld), and as a class with float fields
# updated through `this` (ldarg.0 + ldfld/stfld, with += as dup).
VEC_STRUCT = """
    const int N = 120000;
    struct V3 { public float X, Y, Z; }
    static long Run(int n) {
        V3 p; p.X = 0.5f; p.Y = 1.5f; p.Z = -0.5f;
        V3 v; v.X = 0.1f; v.Y = 0.2f; v.Z = -0.1f;
        V3 a; a.X = 0.3f; a.Y = -0.2f; a.Z = 0.1f;
        V3 nrm; nrm.X = 0.6f; nrm.Y = 0.8f; nrm.Z = 0f;
        float dt = 0.001f, acc = 0f;
        for (int i = 0; i < n; i++) {
            v.X = (v.X + a.X * dt) * 0.9999f; v.Y = (v.Y + a.Y * dt) * 0.9999f; v.Z = (v.Z + a.Z * dt) * 0.9999f;
            p.X = p.X + v.X * dt; p.Y = p.Y + v.Y * dt; p.Z = p.Z + v.Z * dt;
            float d = p.X * nrm.X + p.Y * nrm.Y + p.Z * nrm.Z;
            acc = acc + d * 0.0001f;
        }
        return (long)(acc * 65536f) + (long)(p.X * 1000f);
    }
"""

VEC_CLASS = """
    const int N = 120000;
    class P {
        public float X, Y, Z, VX, VY, VZ;
        public void Step(float dt) {
            X += VX * dt; Y += VY * dt; Z += VZ * dt;
            VX *= 0.9999f; VY *= 0.9999f; VZ = VZ - 9.8f * dt;
        }
    }
    static long Run(int n) {
        P[] ps = new P[64];
        for (int i = 0; i < ps.Length; i++) { ps[i] = new P(); ps[i].X = i; ps[i].VX = 0.5f; ps[i].VY = -0.25f; ps[i].VZ = 2f; }
        for (int s = 0; s < n / 64; s++) for (int i = 0; i < ps.Length; i++) ps[i].Step(0.001f);
        float sum = 0f;
        for (int i = 0; i < ps.Length; i++) sum += ps[i].X + ps[i].Y + ps[i].Z;
        return (long)(sum * 1000f);
    }
"""

# A bouncing particle: float compares that clamp and reverse (every one used to end a native block), and an int-to-float
# conversion in the loop.
VEC_BOUNCE = """
    const int N = 150000;
    static long Run(int n) {
        float x = 0f, y = 5f, vx = 1.5f, vy = -0.7f, dt = 0.01f;
        int bounces = 0;
        for (int i = 0; i < n; i++) {
            x = x + vx * dt; y = y + vy * dt;
            if (x > 10f) { x = 10f; vx = -vx; bounces++; }
            if (x < -10f) { x = -10f; vx = -vx; bounces++; }
            if (y < 0f) { y = 0f; vy = -vy * 0.9f; bounces++; }
            vy = vy - 9.8f * dt;
            float t = (float)i * 0.0001f;
            x = x + t * 0.0f;
        }
        return (long)(x * 1000f) + (long)(y * 1000f) + bounces;
    }
"""

# Particles as arrays: structure of arrays (float[] positions and velocities), the layout game code uses for bulk updates, and an
# array of structs (ldelema + a field), the other common one.
VEC_ARRAY = """
    const int N = 2000;
    static long Run(int n) {
        int count = 256;
        float[] px = new float[count], py = new float[count], vx = new float[count], vy = new float[count];
        for (int i = 0; i < count; i++) { px[i] = i * 0.5f; py[i] = 100f - i; vx[i] = 1f + i * 0.01f; vy[i] = -0.5f; }
        float dt = 0.01f;
        for (int step = 0; step < n / 16; step++) {
            for (int i = 0; i < px.Length; i++) {
                px[i] = px[i] + vx[i] * dt;
                py[i] = py[i] + vy[i] * dt;
                vy[i] = vy[i] - 9.8f * dt;
                if (py[i] < 0f) { py[i] = 0f; vy[i] = -vy[i] * 0.9f; }
            }
        }
        float sum = 0f;
        for (int i = 0; i < px.Length; i++) sum = sum + px[i] + py[i];
        return (long)(sum * 100f);
    }
"""

VEC_AOS = """
    const int N = 2000;
    struct P { public float X, Y, VX, VY; }
    static long Run(int n) {
        int count = 256;
        P[] ps = new P[count];
        for (int i = 0; i < count; i++) { ps[i].X = i * 0.5f; ps[i].Y = 100f - i; ps[i].VX = 1f + i * 0.01f; ps[i].VY = -0.5f; }
        float dt = 0.01f;
        for (int step = 0; step < n / 16; step++) {
            for (int i = 0; i < ps.Length; i++) {
                ps[i].X = ps[i].X + ps[i].VX * dt;
                ps[i].Y = ps[i].Y + ps[i].VY * dt;
                ps[i].VY = ps[i].VY - 9.8f * dt;
                if (ps[i].Y < 0f) { ps[i].Y = 0f; ps[i].VY = -ps[i].VY * 0.9f; }
            }
        }
        float sum = 0f;
        for (int i = 0; i < ps.Length; i++) sum = sum + ps[i].X + ps[i].Y;
        return (long)(sum * 100f);
    }
"""

INT_LOOP = """
    const int N = 2000000;
    static long Run(int n) {
        long s = 0;
        for (int i = 0; i < n; i++) {
            s += (i * 7) ^ (i >> 3);
            s ^= i;
        }
        return s;
    }
"""

DOUBLE_LOOP = """
    const int N = 800000;
    static long Run(int n) {
        double x = 0, y = 1;
        for (int i = 1; i <= n; i++) {
            x += 1.0 / ((double)i * i);
            y = y * 0.999999 + x;
        }
        return (long)(x * 1e9) + (long)y;
    }
"""

RECURSION = """
    const int N = 24;
    static int Fib(int n) { return n < 2 ? n : Fib(n - 1) + Fib(n - 2); }
    static long Run(int n) { return Fib(n == 24 ? 24 : 12); }
"""

SIEVE = """
    const int N = 1000000;
    static long Run(int n) {
        bool[] composite = new bool[n + 1];
        long count = 0;
        for (int i = 2; i <= n; i++) {
            if (!composite[i]) {
                count++;
                for (long j = (long)i * i; j <= n; j += i) composite[(int)j] = true;
            }
        }
        return count;
    }
"""

VIRTUAL_CALLS = """
    const int N = 600000;
    interface IScale { int Scale(int x); }
    abstract class Shape { public abstract int Area(int x); }
    class Square : Shape, IScale { public override int Area(int x) { return x * x; } public int Scale(int x) { return x + 1; } }
    class Rect : Shape, IScale { public override int Area(int x) { return x * 2; } public int Scale(int x) { return x + 2; } }
    class Tri : Shape, IScale { public override int Area(int x) { return x / 2 + 1; } public int Scale(int x) { return x ^ 3; } }
    static long Run(int n) {
        Shape[] shapes = new Shape[] { new Square(), new Rect(), new Tri(), new Rect() };
        long s = 0;
        for (int i = 0; i < n; i++) {
            Shape sh = shapes[i & 3];
            s += sh.Area(i & 255);
            s += ((IScale)sh).Scale(i & 255);
        }
        return s;
    }
"""

DELEGATES = """
    const int N = 500000;
    static int Twice(int x) { return x * 2; }
    static int Inc(int x) { return x + 1; }
    static long Run(int n) {
        Func<int, int> a = Twice, b = Inc;
        Func<int, int> c = x => x ^ 5;
        long s = 0;
        for (int i = 0; i < n; i++) {
            s += a(i & 1023);
            s += b(i & 1023);
            s += c(i & 1023);
        }
        return s;
    }
"""

STRUCT_MATH = """
    const int N = 300000;
    struct Vec {
        public double X, Y, Z;
        public Vec(double x, double y, double z) { X = x; Y = y; Z = z; }
        public static Vec Add(Vec a, Vec b) { return new Vec(a.X + b.X, a.Y + b.Y, a.Z + b.Z); }
        public static Vec Scale(Vec a, double k) { return new Vec(a.X * k, a.Y * k, a.Z * k); }
        public double Dot(Vec b) { return X * b.X + Y * b.Y + Z * b.Z; }
    }
    static long Run(int n) {
        Vec p = new Vec(1, 2, 3), v = new Vec(0.5, -0.25, 0.125);
        double d = 0;
        for (int i = 0; i < n; i++) {
            p = Vec.Add(p, Vec.Scale(v, 0.001));
            d += p.Dot(v);
        }
        return (long)(d * 1000) + (long)p.X;
    }
"""

ALLOC = """
    const int N = 300000;
    class Node { public Node Next; public int V; }
    static long Run(int n) {
        Node head = null;
        long s = 0;
        for (int i = 0; i < n; i++) {
            Node x = new Node();
            x.V = i;
            x.Next = (i % 64 == 0) ? null : head;   // chains of 64: lots of short-lived garbage
            head = x;
            s += x.V;
        }
        return s + head.V;
    }
"""

BOXING = """
    const int N = 300000;
    static long Run(int n) {
        long s = 0;
        object o = null;
        for (int i = 0; i < n; i++) {
            o = i;
            s += (int)o;
        }
        return s;
    }
"""

LIST_INT = """
    const int N = 300000;
    static long Run(int n) {
        List<int> list = new List<int>();
        for (int i = 0; i < n; i++) list.Add(i * 3);
        long s = 0;
        for (int i = 0; i < list.Count; i++) s += list[i];
        return s;
    }
"""

DICTIONARY = """
    const int N = 60000;
    static long Run(int n) {
        Dictionary<int, int> d = new Dictionary<int, int>();
        for (int i = 0; i < n; i++) d[i * 7] = i;
        long s = 0;
        for (int i = 0; i < n; i++) { int v; if (d.TryGetValue(i * 7, out v)) s += v; }
        return s + d.Count;
    }
"""

MATH_CALLS = """
    const int N = 300000;
    static long Run(int n) {
        double s = 0;
        for (int i = 0; i < n; i++) s += Math.Sin(i * 0.001) + Math.Sqrt(i);
        return (long)(s * 100);
    }
"""

STRING_CONCAT = """
    const int N = 8000;
    static long Run(int n) {
        string s = "";
        for (int i = 0; i < n; i++) s = s + "ab";   // each step copies the whole string: mostly memcpy
        return s.Length + s[s.Length - 1];
    }
"""

ARRAY_COPY = """
    const int N = 100;
    static long Run(int n) {
        byte[] a = new byte[1 << 20], b = new byte[1 << 20];
        for (int i = 0; i < a.Length; i += 4096) a[i] = (byte)i;
        for (int r = 0; r < n; r++) Array.Copy(a, b, a.Length);   // one memmove of 1 MB each
        return b[4096] + b[8192] + b.Length;
    }
"""

EXCEPTIONS = """
    const int N = 2000;
    static void Thrower(int depth) {
        if (depth == 0) throw new InvalidOperationException("boom");
        Thrower(depth - 1);
    }
    static long Run(int n) {
        long c = 0;
        for (int i = 0; i < n; i++) {
            try { Thrower(4); }
            catch (InvalidOperationException) { c += 1; }
            finally { c += 2; }
        }
        return c;
    }
"""

# Many methods that each run exactly once: the cost is compiling them, not running them. (Generated by cold_source().)
COLD_METHODS = """
    const int N = 1;
/*METHODS*/
    static long Run(int n) {
        long s = 0;
        int i = n;
/*CALLS*/
        return s;
    }
"""
COLD_COUNT = 300


def cold_source():
    methods = "\n".join(
        "    static int M%d(int x) { int a = x * %d; if (a > 100) a -= %d; "
        "for (int j = 0; j < 2; j++) a += (j ^ x) + %d; return a; }" % (k, k + 3, k, k % 7)
        for k in range(COLD_COUNT))
    calls = "\n".join("        s += M%d(i);" % k for k in range(COLD_COUNT))
    return COLD_METHODS.replace("/*METHODS*/", methods).replace("/*CALLS*/", calls)


# name, one line on what it measures, C# body, warm up first?, measured from outside (process start-up)?
# [DllImport] of a C function: on DNA the runtime built with tests/ffi/mylib.json (build.py --ffi), on Mono the same C as libmylib.so
FFI_ADD = """
    [DllImport("mylib")] static extern int add_numbers(int a, int b);
    const int N = 2000000;
    static long Run(int n) { long s = 0; for (int i = 0; i < n; i++) { s += add_numbers(i, 3); } return s; }
"""
FFI_SUM6 = """
    [DllImport("mylib")] static extern int sum6(int a, int b, int c, int d, int e, int f);
    const int N = 2000000;
    static long Run(int n) { long s = 0; for (int i = 0; i < n; i++) { s += sum6(i, 1, 2, 3, 4, i & 7); } return s; }
"""
FFI_MIXED = """
    [DllImport("mylib")] static extern double mixed(int a, double b, long c, float d, int e);
    const int N = 2000000;
    static long Run(int n) { double s = 0; for (int i = 0; i < n; i++) { s += mixed(i, 0.5, 7L, 0.25f, 3); } return (long)s; }
"""

FFI_BUF = """
    [DllImport("mylib")] static extern int sum_buf(int[] p, int n);
    const int N = 500000;
    static int[] arr = new int[16];
    static long Run(int n) { for (int i = 0; i < 16; i++) arr[i] = i * 3 + 1; long s = 0; for (int i = 0; i < n; i++) s += sum_buf(arr, 16); return s; }
"""
FFI_REF = """
    [DllImport("mylib")] static extern void swap_ref(ref int a, ref int b);
    const int N = 500000;
    static long Run(int n) { int a = 1, b = 2; long s = 0; for (int i = 0; i < n; i++) { swap_ref(ref a, ref b); s += a; } return s; }
"""
FFI_STR = """
    [DllImport("mylib")] static extern int str_len(string s);
    const int N = 500000;
    static long Run(int n) { string t = "hello world"; long s = 0; for (int i = 0; i < n; i++) s += str_len(t); return s; }
"""
FFI_ECHO = """
    [DllImport("mylib")] static extern string echo_upper(string s);
    const int N = 200000;
    static long Run(int n) { string t = "hello"; long s = 0; for (int i = 0; i < n; i++) s += echo_upper(t).Length; return s; }
"""

BENCHMARKS = [
    ("startup",       "process start: a program that returns 0 (whole run, from outside)", STARTUP, False, True),
    ("vec_inline",    "float32 physics step inline: long runs of float arithmetic on locals", VEC_INLINE, True, False),
    ("vec_calls",     "float32 dot / lerp / quaternion-rotate called as small methods",       VEC_CALLS, True, False),
    ("vec_struct",    "Vector3 as a struct in locals: ldloca + ldfld/stfld on every access",   VEC_STRUCT, True, False),
    ("vec_class",     "particles as objects: Step() updates float fields through this",        VEC_CLASS, True, False),
    ("vec_array",     "particles in float[] arrays (structure of arrays): integrate and bounce",   VEC_ARRAY, True, False),
    ("vec_aos",       "particles as an array of structs (ldelema + a field): integrate and bounce", VEC_AOS, True, False),
    ("vec_bounce",    "a bouncing particle: float compares that clamp and reverse, int to float", VEC_BOUNCE, True, False),
    ("cold_methods",  "300 methods each run once: compile cost, no warm-up",               None,    False, False),
    ("exceptions",    "throw / catch / finally through 5 frames",                           EXCEPTIONS, True, False),
    ("array_copy",    "Array.Copy of 1 MB, 100 times (one memmove each)",                   ARRAY_COPY, True, False),
    ("string_concat", "s = s + \"ab\" 8000 times (mostly memcpy)",                         STRING_CONCAT, True, False),
    ("alloc",         "300k small objects, chains of 64 (GC and allocator)",                ALLOC, True, False),
    ("boxing",        "box and unbox an int, 300k times",                                   BOXING, True, False),
    ("math_calls",    "Math.Sin and Math.Sqrt, 300k times",                                 MATH_CALLS, True, False),
    ("delegates",     "Func<int,int> calls: method group, method group, lambda",            DELEGATES, True, False),
    ("virtual_calls", "virtual and interface calls over 4 receiver types",                  VIRTUAL_CALLS, True, False),
    ("struct_math",   "a 3-vector struct passed and returned by value",                     STRUCT_MATH, True, False),
    ("recursion",     "fib(24): about 75k calls",                                           RECURSION, True, False),
    ("dictionary",    "Dictionary<int,int>: 60k inserts and lookups",                       DICTIONARY, True, False),
    ("list_int",      "List<int>: 300k adds and indexed reads",                             LIST_INT, True, False),
    ("sieve",         "sieve of Eratosthenes over 1M bools (array access)",                 SIEVE, True, False),
    ("double_loop",   "800k iterations of double arithmetic",                               DOUBLE_LOOP, True, False),
    ("int_loop",      "2M iterations of integer arithmetic",                                INT_LOOP, True, False),
    ("ffi_add",       "[DllImport] add_numbers(int, int), 2M calls (needs build.py --ffi)",         FFI_ADD, True, False),
    ("ffi_sum6",      "[DllImport] six integer arguments, 2M calls",                              FFI_SUM6, True, False),
    ("ffi_mixed",     "[DllImport] int, double, long, float, int arguments, double result, 2M calls", FFI_MIXED, True, False),
    ("ffi_buf",       "[DllImport] an int[] argument (a pointer to its elements), 500k calls",    FFI_BUF, True, False),
    ("ffi_ref",       "[DllImport] two ref arguments, 500k calls",                                FFI_REF, True, False),
    ("ffi_str",       "[DllImport] a string argument (a temporary UTF-8 copy), 500k calls",       FFI_STR, True, False),
    ("ffi_echo",      "[DllImport] a string in and a string out, 200k calls",                     FFI_ECHO, True, False),
]


def program(name, body, warm):
    if name == "startup":
        return STARTUP
    if body is None:
        body = cold_source()
    return MAIN.replace("/*BODY*/", body).replace("/*WARM*/", WARMUP if warm else "")


# ---------------------------------------------------------------------------------------------------------------

def sh(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, **kw)


def net8_tools():
    """(dotnet, csc.dll, reference assemblies dir, runtime version) for the .NET SDK, or None"""
    dotnet = shutil.which("dotnet")
    csc = (glob.glob("/usr/lib/dotnet/sdk/*/Roslyn/bincore/csc.dll") or glob.glob("/usr/share/dotnet/sdk/*/Roslyn/bincore/csc.dll") or [None])[0]
    refs = (glob.glob("/usr/lib/dotnet/packs/Microsoft.NETCore.App.Ref/*/ref/net*") or glob.glob("/usr/share/dotnet/packs/Microsoft.NETCore.App.Ref/*/ref/net*") or [None])[0]
    rts = sorted(glob.glob("/usr/lib/dotnet/shared/Microsoft.NETCore.App/*") + glob.glob("/usr/share/dotnet/shared/Microsoft.NETCore.App/*"))
    if not (dotnet and csc and refs and rts):
        return None
    return dotnet, csc, refs, os.path.basename(rts[-1])


def compile_net8(name, source):
    """Compile `source` with Roslyn for .NET 8. Returns ([command to run it], error or None)."""
    dotnet, csc, refs, version = net8_tools()
    d = os.path.join(WORK, "net8")
    os.makedirs(d, exist_ok=True)
    cs, dll, cfg = (os.path.join(d, name + ext) for ext in (".cs", ".dll", ".runtimeconfig.json"))
    open(cs, "w").write(source)
    cmd = [dotnet, csc, "-noconfig", "-nologo", "-unsafe", "-optimize+", "-nullable:disable", "-nowarn:0219,0414", "-target:exe", "-out:" + dll, cs]
    cmd += ["-r:" + f for f in glob.glob(os.path.join(refs, "*.dll"))]
    r = sh(cmd)
    if r.returncode or not os.path.exists(dll):
        return None, ((r.stdout + r.stderr).strip().splitlines() or ["compile failed"])[0][:160]
    open(cfg, "w").write('{"runtimeOptions":{"tfm":"net8.0","framework":{"name":"Microsoft.NETCore.App","version":"%s"}}}' % version)
    return [dotnet, "exec", "--runtimeconfig", cfg, dll], None


def compile_one(kind, name, source, dna_corlib):
    """Compile `source` for 'mono' or 'dna'. Returns (exe path, error or None). Cached by a hash of the source."""
    d = os.path.join(WORK, kind)
    os.makedirs(d, exist_ok=True)
    cs, exe, stamp = (os.path.join(d, name + ext) for ext in (".cs", ".exe", ".hash"))
    key = kind + source
    if kind == "dna":
        key += hashlib.sha1(open(dna_corlib, "rb").read()).hexdigest()     # a changed corlib invalidates the cache
    h = hashlib.sha1(key.encode()).hexdigest()
    if os.path.exists(exe) and os.path.exists(stamp) and open(stamp).read() == h:
        return exe, None
    open(cs, "w").write(source)
    if kind == "dna":
        cmd = ["mcs", "-nostdlib", "-optimize+", "-nowarn:0219,0414", "-r:" + dna_corlib, "-out:" + exe, cs]
    else:
        cmd = ["mcs", "-optimize+", "-nowarn:0219,0414", "-out:" + exe, cs]
    r = sh(cmd)
    if r.returncode != 0 or not os.path.exists(exe):
        return exe, ((r.stdout + r.stderr).strip().splitlines() or ["compile failed"])[0][:160]
    open(stamp, "w").write(h)
    return exe, None


def run_once(cmd, cwd, timeout, env=None):
    """Run a command: (wall seconds, {'SUM': .., 'US': ..}, error or None)"""
    t0 = time.perf_counter()
    try:
        r = sh(cmd, cwd=cwd, timeout=timeout, env=env)
    except subprocess.TimeoutExpired:
        return None, {}, "timed out after %ds" % timeout
    wall = time.perf_counter() - t0
    out = {}
    for line in r.stdout.splitlines():
        parts = line.split()
        if len(parts) == 2 and parts[0] in ("SUM", "US"):
            out[parts[0]] = parts[1]
    if r.returncode != 0:
        text = (r.stdout + r.stderr).strip().splitlines()
        return wall, out, "exit %d: %s" % (r.returncode, (text[-1] if text else "")[:100])
    return wall, out, None


def measure(runtime, exe, external, repeat, timeout, ffi=False):
    """Best of `repeat` runs. Returns {'us': best microseconds, 'sum': checksum} or {'error': ..}"""
    cwd = os.path.dirname(exe if isinstance(exe, str) else exe[-1])
    cmd = {"dna": [FFI_BIN if ffi else DNA_BIN, exe], "mono": ["mono", exe], "mono-interp": ["mono", "--interpreter", exe],
           "net8": exe}[runtime]
    env = dict(os.environ, LD_LIBRARY_PATH=FFI_LIBDIR) if ffi and runtime != "dna" else None      # (where Mono finds libmylib.so)
    best, checksum = None, None
    for _ in range(repeat * (3 if external else 1)):
        wall, out, err = run_once(cmd, cwd, timeout, env)
        if err:
            return {"error": err}
        us = wall * 1e6 if external else float(out.get("US", "nan"))
        if best is None or us < best:
            best = us
        checksum = out.get("SUM", checksum)
    return {"us": best, "sum": checksum}


def verdict(ratio):
    if ratio is None:
        return "n/a"
    if ratio < 0.9:
        return "DNA %.3gx faster" % (1 / ratio)
    if ratio > 1.1:
        return "DNA %.3gx slower" % ratio
    return "about the same"


def print_table(rows, with_interp, with_net8=False):
    print()
    print("  %-14s %10s %s%s%10s %11s   %s" % ("benchmark", "Mono ms", "%14s " % "Mono-interp ms" if with_interp else "",
                                           "%10s " % ".NET 8 ms" if with_net8 else "", "DNA ms", "DNA/Mono", "verdict"))
    for r in rows:
        if "error" in r:
            print("  %-14s  %s" % (r["name"], r["error"]))
            continue
        extra = ("%14s " % ("%.2f" % (r["mono_interp_us"] / 1000) if r.get("mono_interp_us") else "-")) if with_interp else ""
        note = "" if r.get("same", True) else "   !! checksums differ (Mono %s, DNA %s)" % (r["mono_sum"], r["dna_sum"])
        n8 = ("%10s " % ("%.2f" % (r["net8_us"] / 1000) if r.get("net8_us") else "-")) if with_net8 else ""
        n8note = ("   (%.3gx .NET 8)" % (r["dna_us"] / r["net8_us"])) if with_net8 and r.get("net8_us") else ""
        print("  %-14s %10.2f %s%s%10.2f %10.3gx   %s%s%s" % (r["name"], r["mono_us"] / 1000, extra, n8, r["dna_us"] / 1000,
                                                          r["ratio"], verdict(r["ratio"]), n8note, note))


def plot(rows, meta, out_png, show, with_interp):
    try:
        import matplotlib
        if not show:
            matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        print("matplotlib is not installed, so there is no chart (pip install matplotlib)")
        return
    good = sorted((r for r in rows if "error" not in r), key=lambda r: r["ratio"])
    if not good:
        return
    names = [r["name"] for r in good]
    ys = list(range(len(good)))
    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(14, 0.52 * len(good) + 2.6), gridspec_kw={"width_ratios": [1.15, 1]})

    # left: time, log scale
    h = 0.27 if with_interp else 0.36
    ax1.barh([y + h * (1 if with_interp else 0.5) for y in ys], [r["mono_us"] / 1000 for r in good], h, label="Mono (JIT)", color="#5b8def")
    if with_interp:
        ax1.barh(ys, [(r.get("mono_interp_us") or float("nan")) / 1000 for r in good], h, label="Mono (--interpreter)", color="#9aa5b5")
    ax1.barh([y - h * (1 if with_interp else 0.5) for y in ys], [r["dna_us"] / 1000 for r in good], h, label="DNA", color="#f08a24")
    ax1.set_xscale("log")
    ax1.set_yticks(ys)
    ax1.set_yticklabels(names)
    ax1.set_xlabel("time in ms (log scale; shorter is faster)")
    ax1.legend(loc="lower right")
    ax1.grid(axis="x", alpha=0.3, which="both")
    ax1.set_title("Time")

    # right: how many times slower or faster than Mono
    ratios = [r["ratio"] for r in good]
    colors = ["#2e9e4f" if x < 0.9 else ("#d6453d" if x > 1.1 else "#8a8a8a") for x in ratios]
    # bars grow from the 1x line: to the right when DNA is slower, to the left when it is faster
    ax2.barh(ys, [x - 1.0 for x in ratios], 0.6, left=1.0, color=colors)
    ax2.set_xscale("log")
    ax2.axvline(1.0, color="black", lw=1)
    ax2.set_yticks(ys)
    ax2.set_yticklabels([])
    ax2.set_xlabel("DNA time / Mono time (log scale; left of the line = DNA faster)")
    ax2.grid(axis="x", alpha=0.3, which="both")
    lo, hi = min(ratios), max(ratios)
    ax2.set_xlim(min(lo / 7.0, 0.3), max(hi * 4.0, 3.0))      # room for the labels beyond the bars
    for y, x in zip(ys, ratios):
        label = ("%.3gx faster" % (1 / x)) if x < 0.9 else (("%.3gx slower" % x) if x > 1.1 else "same")
        ax2.text(x * (1.12 if x >= 1 else 1 / 1.12), y, label, va="center", ha="left" if x >= 1 else "right", fontsize=9)
    ax2.set_title("DNA relative to Mono")

    fig.suptitle("DNA vs Mono: %s   (best of %d; %s)" % (meta["dna"], meta["repeat"], meta["mono"]), fontsize=12)
    fig.tight_layout()
    os.makedirs(os.path.dirname(out_png), exist_ok=True)
    fig.savefig(out_png, dpi=130)
    print("chart written to", os.path.relpath(out_png, ROOT) if out_png.startswith(ROOT) else out_png)
    if show:
        plt.show()


def main():
    global DNA_BIN
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dna", help="the runtime to benchmark (default build/dna, else build/dna32)")
    ap.add_argument("--repeat", type=int, default=3, help="runs per benchmark; the best is used (default 3)")
    ap.add_argument("--only", help="comma-separated benchmark names")
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--mono-interp", action="store_true", help="also run Mono's interpreter (mono --interpreter)")
    ap.add_argument("--net8", action="store_true", help="also run .NET 8 (compiled with Roslyn; needs the .NET SDK): a stricter baseline than Mono")
    ap.add_argument("--timeout", type=int, default=60, help="seconds allowed per run (default 60)")
    ap.add_argument("--out", default=os.path.join(BUILD, "benchmark_mono.png"), help="chart file")
    ap.add_argument("--json", help="also write the results here")
    ap.add_argument("--plot-only", metavar="JSON", help="draw the chart from saved results, running nothing")
    ap.add_argument("--show", action="store_true", help="show the chart in a window")
    ap.add_argument("--no-plot", action="store_true")
    args = ap.parse_args()

    if args.list:
        for name, what, *_ in BENCHMARKS:
            print("  %-14s %s" % (name, what))
        return 0

    if args.plot_only:
        saved = json.load(open(args.plot_only))
        print_table(saved["rows"], saved["meta"].get("mono_interp", False), any(r.get("net8_us") for r in saved["rows"]))
        plot(saved["rows"], saved["meta"], args.out, args.show, saved["meta"].get("mono_interp", False))
        return 0

    for tool in ("mono", "mcs"):
        if not shutil.which(tool):
            print("error: %s not found (apt install mono-runtime mono-mcs)" % tool, file=sys.stderr)
            return 1
    DNA_BIN = args.dna or next((p for p in (os.path.join(BUILD, "dna"), os.path.join(BUILD, "dna32")) if os.path.exists(p)), None)
    corlib = os.path.join(BUILD, "corlib.dll")
    if not DNA_BIN or not os.path.exists(DNA_BIN) or not os.path.exists(corlib):
        print("error: build the runtime and corlib first (make)", file=sys.stderr)
        return 1
    DNA_BIN = os.path.abspath(DNA_BIN)

    chosen = [b for b in BENCHMARKS if not args.only or b[0] in args.only.split(",")]
    if not chosen:
        print("error: no such benchmark; --list shows them", file=sys.stderr)
        return 1

    if any(b[0].startswith("ffi_") for b in chosen):
        if not build_ffi():
            return 1

    net8_cmds = {}
    if args.net8:
        if net8_tools() is None:
            print("error: --net8 needs the .NET SDK (dotnet, Roslyn, the reference assemblies)", file=sys.stderr)
            return 1
        for name, what, body, warm, external in chosen:
            net8_cmds[name] = compile_net8(name, program(name, body, warm))

    # compile everything for both runtimes, in parallel
    os.makedirs(WORK, exist_ok=True)
    jobs = []
    t0 = time.time()
    with cf.ThreadPoolExecutor(os.cpu_count() or 2) as ex:
        for name, what, body, warm, external in chosen:
            src = program(name, body, warm)
            for kind in ("mono", "dna"):
                jobs.append((name, kind, ex.submit(compile_one, kind, name, src, corlib)))
    exes = {}
    for name, kind, fut in jobs:
        exes[(name, kind)] = fut.result()
    os.makedirs(os.path.join(WORK, "dna"), exist_ok=True)
    shutil.copy(corlib, os.path.join(WORK, "dna", "corlib.dll"))
    print("compiled %d programs in %.1fs" % (len(jobs), time.time() - t0))

    mono_ver = (sh(["mono", "--version"]).stdout.splitlines() or ["mono"])[0].replace("Mono JIT compiler version ", "Mono ").split(" (")[0]
    rows = []
    t0 = time.time()
    for name, what, body, warm, external in chosen:
        (mexe, merr), (dexe, derr) = exes[(name, "mono")], exes[(name, "dna")]
        row = {"name": name, "what": what, "external": external}
        if merr or derr:
            row["error"] = "does not compile for %s: %s" % ("Mono" if merr else "DNA", merr or derr)
            rows.append(row)
            continue
        isffi = name.startswith("ffi_")
        m = measure("mono", mexe, external, args.repeat, args.timeout, isffi)
        d = measure("dna", dexe, external, args.repeat, args.timeout, isffi)
        if "error" in m or "error" in d:
            row["error"] = "failed: Mono %s; DNA %s" % (m.get("error", "ok"), d.get("error", "ok"))
            rows.append(row)
            continue
        row.update(mono_us=m["us"], dna_us=d["us"], mono_sum=m["sum"], dna_sum=d["sum"],
                   same=(m["sum"] == d["sum"]), ratio=d["us"] / m["us"])
        if args.net8:
            ncmd, nerr = net8_cmds[name]
            if nerr:
                row["net8_us"] = None
            else:
                n8 = measure("net8", ncmd, external, args.repeat, args.timeout, isffi)
                row["net8_us"] = n8.get("us")
        if args.mono_interp:
            mi = measure("mono-interp", mexe, external, args.repeat, args.timeout, isffi)
            row["mono_interp_us"] = mi.get("us")
        rows.append(row)
        sys.stdout.write(".")
        sys.stdout.flush()
    print(" ran in %.1fs" % (time.time() - t0))

    print_table(rows, args.mono_interp, args.net8)
    meta = {"dna": os.path.relpath(DNA_BIN, ROOT) if DNA_BIN.startswith(ROOT) else DNA_BIN, "repeat": args.repeat,
            "mono": mono_ver, "mono_interp": args.mono_interp, "date": datetime.date.today().isoformat()}
    if args.json:
        json.dump({"meta": meta, "rows": rows}, open(args.json, "w"), indent=1)
    bad = [r["name"] for r in rows if "error" not in r and not r["same"]]
    if bad:
        print("\nnote: the checksum differs from Mono's for: %s" % ", ".join(bad))
    if not args.no_plot:
        plot(rows, meta, args.out, args.show, args.mono_interp)
    return 1 if any("error" in r for r in rows) else 0


DNA_BIN = None
FFI_BIN = None          # the runtime built with tests/ffi/mylib.json, for the ffi_* benchmarks
FFI_LIBDIR = None       # where libmylib.so is, for Mono


def build_ffi():
    """Build what the ffi_* benchmarks need: the runtime with the manifest (build/dna_ffi) and the C library for Mono."""
    global FFI_BIN, FFI_LIBDIR
    manifest = os.path.join(ROOT, "tests", "ffi", "mylib.json")
    r = sh([sys.executable, os.path.join(ROOT, "build.py"), "--ffi", manifest, "--no-corlib"])
    FFI_BIN = os.path.join(BUILD, "dna_ffi")
    if r.returncode or not os.path.exists(FFI_BIN):
        print("error: the runtime with the FFI manifest did not build:\n" + (r.stdout + r.stderr)[-500:], file=sys.stderr)
        return False
    FFI_LIBDIR = os.path.join(BUILD, "ffi")
    os.makedirs(FFI_LIBDIR, exist_ok=True)
    r = sh(["gcc", "-shared", "-fPIC", "-O2", "-o", os.path.join(FFI_LIBDIR, "libmylib.so"), os.path.join(ROOT, "tests", "ffi", "mylib.c")])
    if r.returncode:
        print("error: libmylib.so did not build:\n" + r.stderr[-500:], file=sys.stderr)
        return False
    return True

if __name__ == "__main__":
    sys.exit(main())
