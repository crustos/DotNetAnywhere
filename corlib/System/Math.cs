// Copyright (c) 2012 DotNetAnywhere
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

using System.Runtime.CompilerServices;

#if LOCALTEST
using System;
namespace System_ {
#else
namespace System {
#endif
	public static class Math {

		public const double E = 2.7182818284590452354;
		public const double PI = 3.14159265358979323846;

		#region Abs()

		public static sbyte Abs(sbyte v) {
			if (v == sbyte.MinValue) {
				throw new OverflowException("Value is too small");
			}
			return (v >= 0) ? v : (sbyte)-v;
		}

		public static short Abs(short v) {
			if (v == short.MinValue) {
				throw new OverflowException("Value is too small");
			}
			return (v >= 0) ? v : (short)-v;
		}

		public static int Abs(int v) {
			if (v == int.MinValue) {
				throw new OverflowException("Value is too small");
			}
			return (v >= 0) ? v : -v;
		}

		public static long Abs(long v) {
			if (v == long.MinValue) {
				throw new OverflowException("Value is too small");
			}
			return (v >= 0) ? v : -v;
		}

		// Clear the sign bit, as the reference runtime does: Abs(-0.0) is +0.0 (the comparison
		// version returned -0.0 for it), and the sign of a NaN is cleared too.
		public static unsafe float Abs(float v) {
			uint bits = *(uint*)&v & 0x7FFFFFFFu;
			return *(float*)&bits;
		}

		public static unsafe double Abs(double v) {
			ulong bits = *(ulong*)&v & 0x7FFFFFFFFFFFFFFFUL;
			return *(double*)&bits;
		}

		#endregion

		#region Min()

		public static sbyte Min(sbyte v1, sbyte v2) {
			return (v1 < v2) ? v1 : v2;
		}

		public static short Min(short v1, short v2) {
			return (v1 < v2) ? v1 : v2;
		}

		public static int Min(int v1, int v2) {
			return (v1 < v2) ? v1 : v2;
		}

		public static long Min(long v1, long v2) {
			return (v1 < v2) ? v1 : v2;
		}

		public static byte Min(byte v1, byte v2) {
			return (v1 < v2) ? v1 : v2;
		}

		public static ushort Min(ushort v1, ushort v2) {
			return (v1 < v2) ? v1 : v2;
		}

		public static uint Min(uint v1, uint v2) {
			return (v1 < v2) ? v1 : v2;
		}

		public static ulong Min(ulong v1, ulong v2) {
			return (v1 < v2) ? v1 : v2;
		}

		// Of two equal values the sign of zero decides: Min(-0, +0) and Min(+0, -0) are both -0.
		// A NaN in either position gives NaN.
		public static float Min(float v1, float v2) {
			if (v1 != v2) {
				if (!float.IsNaN(v1)) {
					return v1 < v2 ? v1 : v2;
				}
				return v1;
			}
			return IsNegative(v1) ? v1 : v2;
		}

		public static double Min(double v1, double v2) {
			if (v1 != v2) {
				if (!double.IsNaN(v1)) {
					return v1 < v2 ? v1 : v2;
				}
				return v1;
			}
			return IsNegative(v1) ? v1 : v2;
		}

		#endregion

		#region Max()

		public static sbyte Max(sbyte v1, sbyte v2) {
			return (v1 > v2) ? v1 : v2;
		}

		public static short Max(short v1, short v2) {
			return (v1 > v2) ? v1 : v2;
		}

		public static int Max(int v1, int v2) {
			return (v1 > v2) ? v1 : v2;
		}

		public static long Max(long v1, long v2) {
			return (v1 > v2) ? v1 : v2;
		}

		public static byte Max(byte v1, byte v2) {
			return (v1 > v2) ? v1 : v2;
		}

		public static ushort Max(ushort v1, ushort v2) {
			return (v1 > v2) ? v1 : v2;
		}

		public static uint Max(uint v1, uint v2) {
			return (v1 > v2) ? v1 : v2;
		}

		public static ulong Max(ulong v1, ulong v2) {
			return (v1 > v2) ? v1 : v2;
		}

		// Of two equal values the sign of zero decides: Max(-0, +0) and Max(+0, -0) are both +0.
		public static float Max(float v1, float v2) {
			if (v1 != v2) {
				if (!float.IsNaN(v1)) {
					return v1 < v2 ? v2 : v1;
				}
				return v1;
			}
			return IsNegative(v2) ? v1 : v2;
		}

		public static double Max(double v1, double v2) {
			if (v1 != v2) {
				if (!double.IsNaN(v1)) {
					return v1 < v2 ? v2 : v1;
				}
				return v1;
			}
			return IsNegative(v2) ? v1 : v2;
		}
		private static bool IsNegative(float f) { return f < 0 || (f == 0 && 1.0f / f < 0); }
		private static bool IsNegative(double d) { return d < 0 || (d == 0 && 1.0 / d < 0); }

		#endregion

		#region Sign()

		public static int Sign(sbyte v) {
			return (v > 0) ? 1 : ((v < 0) ? -1 : 0);
		}

		public static int Sign(short v) {
			return (v > 0) ? 1 : ((v < 0) ? -1 : 0);
		}

		public static int Sign(int v) {
			return (v > 0) ? 1 : ((v < 0) ? -1 : 0);
		}

		public static int Sign(long v) {
			return (v > 0) ? 1 : ((v < 0) ? -1 : 0);
		}

		public static int Sign(float v) {
			if (float.IsNaN(v)) {
				throw new ArithmeticException("NaN");
			}
			return (v > 0) ? 1 : ((v < 0) ? -1 : 0);
		}

		public static int Sign(double v) {
			if (double.IsNaN(v)) {
				throw new ArithmeticException("NaN");
			}
			return (v > 0) ? 1 : ((v < 0) ? -1 : 0);
		}

		#endregion

		[MethodImplAttribute(MethodImplOptions.InternalCall)]
		public extern static double Sin(double x);

		[MethodImplAttribute(MethodImplOptions.InternalCall)]
		public extern static double Cos(double x);

		[MethodImplAttribute(MethodImplOptions.InternalCall)]
		public extern static double Tan(double x);

		[MethodImplAttribute(MethodImplOptions.InternalCall)]
		public extern static double Pow(double x, double y);

		[MethodImplAttribute(MethodImplOptions.InternalCall)]
		public extern static double Sqrt(double x);

		[MethodImplAttribute(MethodImplOptions.InternalCall)]
		public extern static double Asin(double x);

		[MethodImplAttribute(MethodImplOptions.InternalCall)]
		public extern static double Acos(double x);

		[MethodImplAttribute(MethodImplOptions.InternalCall)]
		public extern static double Atan(double x);

		[MethodImplAttribute(MethodImplOptions.InternalCall)]
		public extern static double Sinh(double x);

		[MethodImplAttribute(MethodImplOptions.InternalCall)]
		public extern static double Cosh(double x);

		[MethodImplAttribute(MethodImplOptions.InternalCall)]
		public extern static double Tanh(double x);

		[MethodImplAttribute(MethodImplOptions.InternalCall)]
		public extern static double Exp(double x);

		[MethodImplAttribute(MethodImplOptions.InternalCall)]
		public extern static double Log(double x);

		[MethodImplAttribute(MethodImplOptions.InternalCall)]
		public extern static double Log10(double x);

		[MethodImplAttribute(MethodImplOptions.InternalCall)]
		public extern static double Log2(double x);

		[MethodImplAttribute(MethodImplOptions.InternalCall)]
		public extern static double Floor(double x);

		[MethodImplAttribute(MethodImplOptions.InternalCall)]
		public extern static double Ceiling(double x);

		[MethodImplAttribute(MethodImplOptions.InternalCall)]
		public extern static double Round(double x);

		[MethodImplAttribute(MethodImplOptions.InternalCall)]
		public extern static double Truncate(double x);

		[MethodImplAttribute(MethodImplOptions.InternalCall)]
		public extern static double Cbrt(double x);

		[MethodImplAttribute(MethodImplOptions.InternalCall)]
		public extern static double Atan2(double x, double y);

		[MethodImplAttribute(MethodImplOptions.InternalCall)]
		public extern static double IEEERemainder(double x, double y);

		public static double Log(double a, double newBase) {
			return Log(a) / Log(newBase);
		}

		#region Clamp()

		public static sbyte Clamp(sbyte value, sbyte min, sbyte max) {
			if (min > max) {
				throw new ArgumentException("min is greater than max");
			}
			return (value < min) ? min : ((value > max) ? max : value);
		}

		public static short Clamp(short value, short min, short max) {
			if (min > max) {
				throw new ArgumentException("min is greater than max");
			}
			return (value < min) ? min : ((value > max) ? max : value);
		}

		public static int Clamp(int value, int min, int max) {
			if (min > max) {
				throw new ArgumentException("min is greater than max");
			}
			return (value < min) ? min : ((value > max) ? max : value);
		}

		public static long Clamp(long value, long min, long max) {
			if (min > max) {
				throw new ArgumentException("min is greater than max");
			}
			return (value < min) ? min : ((value > max) ? max : value);
		}

		public static byte Clamp(byte value, byte min, byte max) {
			if (min > max) {
				throw new ArgumentException("min is greater than max");
			}
			return (value < min) ? min : ((value > max) ? max : value);
		}

		public static ushort Clamp(ushort value, ushort min, ushort max) {
			if (min > max) {
				throw new ArgumentException("min is greater than max");
			}
			return (value < min) ? min : ((value > max) ? max : value);
		}

		public static uint Clamp(uint value, uint min, uint max) {
			if (min > max) {
				throw new ArgumentException("min is greater than max");
			}
			return (value < min) ? min : ((value > max) ? max : value);
		}

		public static ulong Clamp(ulong value, ulong min, ulong max) {
			if (min > max) {
				throw new ArgumentException("min is greater than max");
			}
			return (value < min) ? min : ((value > max) ? max : value);
		}

		public static float Clamp(float value, float min, float max) {
			if (min > max) {
				throw new ArgumentException("min is greater than max");
			}
			return (value < min) ? min : ((value > max) ? max : value);
		}

		public static double Clamp(double value, double min, double max) {
			if (min > max) {
				throw new ArgumentException("min is greater than max");
			}
			return (value < min) ? min : ((value > max) ? max : value);
		}
		#endregion
	}
}
