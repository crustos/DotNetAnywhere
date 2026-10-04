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

#if !LOCALTEST

using System.Runtime.CompilerServices;
namespace System.Diagnostics {
	// A monotonic stopwatch. Its ticks are nanoseconds, so Frequency is 1,000,000,000 (as on Mono and .NET on Linux).
	public class Stopwatch {

		public static readonly long Frequency = 1000000000L;
		public static readonly bool IsHighResolution = true;

		[MethodImpl(MethodImplOptions.InternalCall)]
		public extern static long GetTimestamp();

		private long elapsed;           // ticks accumulated while stopped
		private long startTimeStamp;    // when it was last started
		private bool isRunning;

		public Stopwatch() {
		}

		public static Stopwatch StartNew() {
			Stopwatch sw = new Stopwatch();
			sw.Start();
			return sw;
		}

		public bool IsRunning {
			get { return this.isRunning; }
		}

		public void Start() {
			if (!this.isRunning) {
				this.startTimeStamp = GetTimestamp();
				this.isRunning = true;
			}
		}

		public void Stop() {
			if (this.isRunning) {
				this.elapsed += GetTimestamp() - this.startTimeStamp;
				this.isRunning = false;
			}
		}

		public void Reset() {
			this.elapsed = 0;
			this.isRunning = false;
			this.startTimeStamp = 0;
		}

		public void Restart() {
			this.elapsed = 0;
			this.startTimeStamp = GetTimestamp();
			this.isRunning = true;
		}

		public long ElapsedTicks {
			get {
				long t = this.elapsed;
				if (this.isRunning) {
					t += GetTimestamp() - this.startTimeStamp;
				}
				return t;
			}
		}

		public long ElapsedMilliseconds {
			get { return this.ElapsedTicks / (Frequency / 1000L); }
		}

		public TimeSpan Elapsed {
			// a TimeSpan counts 100 ns ticks
			get { return new TimeSpan(this.ElapsedTicks / (Frequency / TimeSpan.TicksPerSecond)); }
		}
	}
}

#endif
