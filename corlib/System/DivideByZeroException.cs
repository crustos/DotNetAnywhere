namespace System {
	public class DivideByZeroException : ArithmeticException {

		public DivideByZeroException() : base("Attempted to divide by zero.") { }
		public DivideByZeroException(string msg) : base(msg) { }

	}
}
