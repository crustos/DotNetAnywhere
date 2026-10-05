// A generic method is called from a different class than the one that declares it, before anything has used that class: the method's own
// class was not known yet (Generics_GetMethodDefFromSpec read it from a method that was never filled), which crashed the runtime.
using System;

class Util {
	public static T Id<T>(T x) { return x; }
	public static int Count<T>() { return 7; }
	public static string Join<A, B>(A a, B b) { return a + "/" + b; }
	public static T First<T>(T[] a) { return a[0]; }
}

class Other {
	public static T Twice<T>(T x) { return Util.Id<T>(Util.Id<T>(x)); }
}

class Program {
	static int Main() {
		Console.WriteLine(Util.Id<int>(3));
		Console.WriteLine(Util.Id<string>("s"));
		Console.WriteLine(Util.Count<long>());
		Console.WriteLine(Util.Join<int, string>(1, "x"));
		Console.WriteLine(Util.First<int>(new int[] { 9, 8 }));
		Console.WriteLine(Other.Twice<string>("t"));
		Console.WriteLine(Other.Twice<int>(5));
		return 0;
	}
}
