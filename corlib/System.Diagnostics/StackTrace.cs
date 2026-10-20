#if !LOCALTEST

using System.Reflection;

namespace System.Diagnostics {
	// ponytail: no stack walking, so a frame does not know its method (programs that only name these still compile and run).
	// Upgrade path: walk the thread's method states (each has its caller and its tMD_MethodDef) in an internal call.
	public class StackTrace {
		public StackTrace() { }
		public virtual int FrameCount { get { return 0; } }
		public virtual StackFrame GetFrame(int index) { return new StackFrame(); }
	}

	public class StackFrame {
		public virtual MethodBase GetMethod() {
			throw new NotSupportedException("DotNetAnywhere does not walk the stack.");
		}
	}
}

#endif
