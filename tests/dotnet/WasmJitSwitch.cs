// The switch instruction, for the wasm JIT (native/src/WasmJIT.c): as a br_table in structured code, as a ladder of blocks in the dispatch loop.
// The output must equal Mono's. With DNA_WASM_JIT_SLICE=1 the loops give up the processor on every pass, so the state machines are left and
// resumed in the middle.
using System;

enum Color : byte { Red, Green, Blue, Alpha = 7 }

class WasmJitSwitch {
    static int Dense(int x) {
        int r = 0;
        switch (x) {
            case 0: r = 10; break;
            case 1: r = 20; break;
            case 2: r = 30; break;
            case 3: r = 40; break;
            case 4: r = 50; break;
            default: r = -1; break;
        }
        return r;
    }
    static int Fallthrough(int x) {
        int r = 0;
        switch (x) {
            case 0: r += 1; goto case 1;
            case 1: r += 10; goto case 2;
            case 2: r += 100; break;
            case 3: case 4: r += 1000; break;
            case 5: r += 5; goto default;
            default: r += 7; break;
        }
        return r;
    }
    static int DefaultOnly(int x) { switch (x) { default: return x * 2; } }
    static int Sparse(int x) { switch (x) { case 1: return 11; case 100: return 22; case 10000: return 33; case -5: return 44; default: return 0; } }
    static int Big(int x) {
        int r = 0;
        switch (x) {
            case 0: r += 3; break;
            case 1: r += 10; break;
            case 2: r += 17; break;
            case 3: r += 24; break;
            case 4: r += 0; break;
            case 5: r += 7; break;
            case 6: r += 14; break;
            case 7: r += 21; break;
            case 8: r += 28; break;
            case 9: r += 4; break;
            case 10: r += 11; break;
            case 11: r += 18; break;
            case 12: r += 25; break;
            case 13: r += 1; break;
            case 14: r += 8; break;
            case 15: r += 15; break;
            case 16: r += 22; break;
            case 17: r += 29; break;
            case 18: r += 5; break;
            case 19: r += 12; break;
            case 20: r += 19; break;
            case 21: r += 26; break;
            case 22: r += 2; break;
            case 23: r += 9; break;
            case 24: r += 16; break;
            case 25: r += 23; break;
            case 26: r += 30; break;
            case 27: r += 6; break;
            case 28: r += 13; break;
            case 29: r += 20; break;
            case 30: r += 27; break;
            case 31: r += 3; break;
            case 32: r += 10; break;
            case 33: r += 17; break;
            case 34: r += 24; break;
            case 35: r += 0; break;
            case 36: r += 7; break;
            case 37: r += 14; break;
            case 38: r += 21; break;
            case 39: r += 28; break;
            case 40: r += 4; break;
            case 41: r += 11; break;
            case 42: r += 18; break;
            case 43: r += 25; break;
            case 44: r += 1; break;
            case 45: r += 8; break;
            case 46: r += 15; break;
            case 47: r += 22; break;
            case 48: r += 29; break;
            case 49: r += 5; break;
            case 50: r += 12; break;
            case 51: r += 19; break;
            case 52: r += 26; break;
            case 53: r += 2; break;
            case 54: r += 9; break;
            case 55: r += 16; break;
            case 56: r += 23; break;
            case 57: r += 30; break;
            case 58: r += 6; break;
            case 59: r += 13; break;
            case 60: r += 20; break;
            case 61: r += 27; break;
            case 62: r += 3; break;
            case 63: r += 10; break;
            case 64: r += 17; break;
            case 65: r += 24; break;
            case 66: r += 0; break;
            case 67: r += 7; break;
            case 68: r += 14; break;
            case 69: r += 21; break;
            case 70: r += 28; break;
            case 71: r += 4; break;
            case 72: r += 11; break;
            case 73: r += 18; break;
            case 74: r += 25; break;
            case 75: r += 1; break;
            case 76: r += 8; break;
            case 77: r += 15; break;
            case 78: r += 22; break;
            case 79: r += 29; break;
            case 80: r += 5; break;
            case 81: r += 12; break;
            case 82: r += 19; break;
            case 83: r += 26; break;
            case 84: r += 2; break;
            case 85: r += 9; break;
            case 86: r += 16; break;
            case 87: r += 23; break;
            case 88: r += 30; break;
            case 89: r += 6; break;
            case 90: r += 13; break;
            case 91: r += 20; break;
            case 92: r += 27; break;
            case 93: r += 3; break;
            case 94: r += 10; break;
            case 95: r += 17; break;
            case 96: r += 24; break;
            case 97: r += 0; break;
            case 98: r += 7; break;
            case 99: r += 14; break;
            case 100: r += 21; break;
            case 101: r += 28; break;
            case 102: r += 4; break;
            case 103: r += 11; break;
            case 104: r += 18; break;
            case 105: r += 25; break;
            case 106: r += 1; break;
            case 107: r += 8; break;
            case 108: r += 15; break;
            case 109: r += 22; break;
            case 110: r += 29; break;
            case 111: r += 5; break;
            case 112: r += 12; break;
            case 113: r += 19; break;
            case 114: r += 26; break;
            case 115: r += 2; break;
            case 116: r += 9; break;
            case 117: r += 16; break;
            case 118: r += 23; break;
            case 119: r += 30; break;
            case 120: r += 6; break;
            case 121: r += 13; break;
            case 122: r += 20; break;
            case 123: r += 27; break;
            case 124: r += 3; break;
            case 125: r += 10; break;
            case 126: r += 17; break;
            case 127: r += 24; break;
            case 128: r += 0; break;
            case 129: r += 7; break;
            case 130: r += 14; break;
            case 131: r += 21; break;
            case 132: r += 28; break;
            case 133: r += 4; break;
            case 134: r += 11; break;
            case 135: r += 18; break;
            case 136: r += 25; break;
            case 137: r += 1; break;
            case 138: r += 8; break;
            case 139: r += 15; break;
            case 140: r += 22; break;
            case 141: r += 29; break;
            case 142: r += 5; break;
            case 143: r += 12; break;
            case 144: r += 19; break;
            case 145: r += 26; break;
            case 146: r += 2; break;
            case 147: r += 9; break;
            case 148: r += 16; break;
            case 149: r += 23; break;
            default: r = -99; break;
        }
        return r;
    }
    static int ReturnInCases(int x) { switch (x) { case 0: return 5; case 1: return 6; case 2: return 7; case 3: return 8; } return -1; }
    static int Nested(int a, int b) {
        int r = 0;
        switch (a) {
            case 0: switch (b) { case 0: r = 1; break; case 1: r = 2; break; case 2: r = 3; break; default: r = 4; break; } break;
            case 1: r = 100; break;
            case 2: switch (b) { case 0: r = 7; break; case 1: r = 8; break; case 2: r = 9; break; } r += 1000; break;
            case 3: r = 55; break;
        }
        return r;
    }
    static int OfColor(Color c) { switch (c) { case Color.Red: return 1; case Color.Green: return 2; case Color.Blue: return 3; case Color.Alpha: return 4; } return 0; }
    static int OfChar(char c) { switch (c) { case 'a': return 1; case 'b': return 2; case 'c': return 3; case 'd': return 4; case 'e': return 5; } return 0; }
    static int OfByte(byte b) { switch (b) { case 250: return 1; case 251: return 2; case 252: return 3; case 253: return 4; } return 0; }
    static int OfUint(uint u) { switch (u) { case 0: return 1; case 1: return 2; case 2: return 3; case 3: return 4; } return 9; }

    // a state machine in a loop: cases that continue, break out of the loop, and change the state
    static int Machine(int n) {
        int state = 0, steps = 0, acc = 0;
        while (steps < n) {
            steps++;
            switch (state) {
                case 0: acc += 1; state = 1; continue;
                case 1: acc += steps; state = (steps % 3 == 0) ? 2 : 3; break;
                case 2: acc ^= 0x55; state = 4; continue;
                case 3: acc -= 2; state = 0; break;
                case 4: acc += 7; if (acc > 100000) return -acc; state = 5; break;
                case 5: state = 0; continue;
                default: return -1;
            }
            acc += 1;
        }
        return acc;
    }
    static int Tokens(int[] input) {            // a switch on data, with a counter per case
        int words = 0, nums = 0, ops = 0, other = 0;
        for (int i = 0; i < input.Length; i++) {
            switch (input[i] & 7) {
                case 0: words++; break; case 1: words += 2; break;
                case 2: case 3: nums++; break;
                case 4: ops++; break;
                default: other++; break;
            }
        }
        return words * 1000000 + nums * 10000 + ops * 100 + other;
    }
    static long SwitchInLoops(int n) { long t = 0; for (int i = 0; i < n; i++) for (int j = 0; j < 4; j++) switch ((i + j) % 5) { case 0: t += i; break; case 1: t -= j; break; case 2: t += 3; break; case 3: t ^= i; break; default: t += 1; break; } return t; }

    static void Main() {
        for (int x = -3; x < 8; x++) Console.WriteLine("small " + x + " " + Dense(x) + " " + Fallthrough(x) + " " + DefaultOnly(x) + " " + ReturnInCases(x));
        int[] sp = { 1, 100, 10000, -5, 0, 2, int.MinValue, int.MaxValue };
        foreach (int x in sp) Console.WriteLine("sparse " + x + " " + Sparse(x) + " " + Dense(x));
        string line = "big"; for (int x = -2; x < 156; x += 7) line += " " + Big(x); Console.WriteLine(line + " " + Big(149) + " " + Big(150) + " " + Big(int.MaxValue) + " " + Big(int.MinValue));
        for (int a = 0; a < 5; a++) for (int b = 0; b < 4; b++) Console.WriteLine("nested " + a + b + " " + Nested(a, b));
        foreach (Color c in new Color[] { Color.Red, Color.Green, Color.Blue, Color.Alpha, (Color)3, (Color)200 }) Console.WriteLine("color " + (int)c + " " + OfColor(c));
        for (char c = 'Z'; c < 'h'; c++) Console.Write(OfChar(c)); Console.WriteLine();
        for (int b = 248; b < 256; b++) Console.Write(OfByte((byte)b)); Console.WriteLine();
        foreach (uint u in new uint[] { 0, 1, 2, 3, 4, uint.MaxValue, 0x80000000u }) Console.Write(OfUint(u) + " "); Console.WriteLine();
        Console.WriteLine("machine " + Machine(0) + " " + Machine(1) + " " + Machine(10) + " " + Machine(100) + " " + Machine(100000));
        int[] data = new int[1000]; for (int i = 0; i < data.Length; i++) data[i] = i * 2654435761u > 0 ? (int)(i * 2654435761u >> 7) : i;
        Console.WriteLine("tokens " + Tokens(data) + " " + Tokens(new int[0]) + " " + SwitchInLoops(50) + " " + SwitchInLoops(0));
    }
}
