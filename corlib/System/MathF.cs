using System.Runtime.CompilerServices;

namespace System {
	// The single-precision counterpart of Math. The transcendental functions call the C library's
	// float versions (sinf, powf, ...) directly, as the reference runtime does, so results match it
	// to the last bit rather than being a double calculation rounded afterwards.
	public static class MathF {
		public const float E = 2.71828183f;
		public const float PI = 3.14159265f;
		public const float Tau = 6.28318531f;

		[MethodImplAttribute(MethodImplOptions.InternalCall)]
		public extern static float Sin(float x);

		[MethodImplAttribute(MethodImplOptions.InternalCall)]
		public extern static float Cos(float x);

		[MethodImplAttribute(MethodImplOptions.InternalCall)]
		public extern static float Tan(float x);

		[MethodImplAttribute(MethodImplOptions.InternalCall)]
		public extern static float Sqrt(float x);

		[MethodImplAttribute(MethodImplOptions.InternalCall)]
		public extern static float Asin(float x);

		[MethodImplAttribute(MethodImplOptions.InternalCall)]
		public extern static float Acos(float x);

		[MethodImplAttribute(MethodImplOptions.InternalCall)]
		public extern static float Atan(float x);

		[MethodImplAttribute(MethodImplOptions.InternalCall)]
		public extern static float Sinh(float x);

		[MethodImplAttribute(MethodImplOptions.InternalCall)]
		public extern static float Cosh(float x);

		[MethodImplAttribute(MethodImplOptions.InternalCall)]
		public extern static float Tanh(float x);

		[MethodImplAttribute(MethodImplOptions.InternalCall)]
		public extern static float Exp(float x);

		[MethodImplAttribute(MethodImplOptions.InternalCall)]
		public extern static float Log(float x);

		[MethodImplAttribute(MethodImplOptions.InternalCall)]
		public extern static float Log10(float x);

		[MethodImplAttribute(MethodImplOptions.InternalCall)]
		public extern static float Log2(float x);

		[MethodImplAttribute(MethodImplOptions.InternalCall)]
		public extern static float Floor(float x);

		[MethodImplAttribute(MethodImplOptions.InternalCall)]
		public extern static float Ceiling(float x);

		[MethodImplAttribute(MethodImplOptions.InternalCall)]
		public extern static float Round(float x);

		[MethodImplAttribute(MethodImplOptions.InternalCall)]
		public extern static float Truncate(float x);

		[MethodImplAttribute(MethodImplOptions.InternalCall)]
		public extern static float Cbrt(float x);

		[MethodImplAttribute(MethodImplOptions.InternalCall)]
		public extern static float Pow(float x, float y);

		[MethodImplAttribute(MethodImplOptions.InternalCall)]
		public extern static float Atan2(float x, float y);

		[MethodImplAttribute(MethodImplOptions.InternalCall)]
		public extern static float IEEERemainder(float x, float y);

		public static float Log(float x, float newBase) {
			return Log(x) / Log(newBase);
		}

		public static float Abs(float x) {
			return Math.Abs(x);
		}

		// As in the reference runtime: a NaN argument gives NaN, and of two equal values the sign of
		// zero decides (Max(-0, +0) is +0, Min(-0, +0) is -0).
		public static float Max(float val1, float val2) {
			if (val1 != val2) {
				if (!float.IsNaN(val1)) {
					return val1 < val2 ? val2 : val1;
				}
				return val1;
			}
			return IsNegative(val2) ? val1 : val2;
		}

		public static float Min(float val1, float val2) {
			if (val1 != val2) {
				if (!float.IsNaN(val1)) {
					return val1 < val2 ? val1 : val2;
				}
				return val1;
			}
			return IsNegative(val1) ? val1 : val2;
		}

		private static bool IsNegative(float f) {
			return f < 0 || (f == 0 && 1.0f / f < 0);
		}

		public static int Sign(float x) {
			if (x < 0) { return -1; }
			if (x > 0) { return 1; }
			if (x == 0) { return 0; }
			throw new ArithmeticException("Function does not accept floating point Not-a-Number values.");
		}
	}
}
