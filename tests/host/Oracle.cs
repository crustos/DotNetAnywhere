// Prints what host.c prints, from Mono.  (Lines starting with # in host.c are checks that have no .NET counterpart.)
using System;
using T;
class Oracle {
	static void P(string name, object v) { Console.WriteLine(name + ": " + v); }
	static void Main() {
		// UTF-8 whatever the locale is, as host.c's printf writes it
		var o = new System.IO.StreamWriter(Console.OpenStandardOutput(), new System.Text.UTF8Encoding(false));
		o.AutoFlush = true;
		Console.SetOut(o);
		P("AddI", Calc.AddI(40, 2));
		P("AddI neg", Calc.AddI(-7, 3));
		P("MulL", Calc.MulL(3000000000L, 7L));
		P("ScaleF", (int)(Calc.ScaleF(1.5f, 4f) * 1000));
		P("Hyp", (long)(Calc.Hyp(3.0, 4.0) * 1000));
		P("IsPos true", Calc.IsPos(5) ? 1 : 0);
		P("IsPos false", Calc.IsPos(-5) ? 1 : 0);
		P("Low", Calc.Low(0x1234));
		P("Narrow", Calc.Narrow(70000));
		P("UClamp", Calc.UClamp(4000000000u, 1u, 3000000000u));
		P("Mixed", Calc.Mixed(1, 2.9, 3L, 4.9f, 5));
		Calc.Nop();
		P("Nop", "done");
		P("Next 1", Calc.Next());
		P("Next 2", Calc.Next());
		P("Next 3", Calc.Next());
		P("Over int", Calc.Over(5));
		P("Over long", Calc.Over(5L));
		P("Greet", Calc.Greet("w\u00f6rld \u2713 \U0001F600"));
		P("Len", Calc.Len("w\u00f6rld \u2713 \U0001F600"));
		P("CodeSum", Calc.CodeSum("w\u00f6rld \u2713 \U0001F600"));
		P("Repeat", Calc.Repeat("ab", 5));
		P("Repeat long", Calc.Repeat("0123456789", 300).Length);
		P("Join3", Calc.Join3("a", "", "c"));
		P("Sum", Calc.Sum(new int[] { 1, 2, 3, 4, 5 }));
		P("Sum empty", Calc.Sum(new int[0]));
		P("SumL", Calc.SumL(new long[] { 4000000000L, 5000000000L, -1L }));
		int[] f = new int[6]; Calc.Fill(f, 10);
		P("Fill", string.Join(",", Array.ConvertAll(f, x => x.ToString())));
		P("Dot", (long)(Calc.Dot(new double[] { 1, 2, 3 }, new double[] { 4, 5, 6 }) * 1000));
		byte[] u = System.Text.Encoding.ASCII.GetBytes("hello, World 42"); Calc.Upper(u);
		P("Upper", System.Text.Encoding.ASCII.GetString(u));
		P("Bytes", string.Join(",", Array.ConvertAll(Calc.Bytes(6), x => x.ToString())));
		P("Squares", string.Join(",", Array.ConvertAll(Calc.Squares(7), x => x.ToString())));
		P("Halves", string.Join(",", Array.ConvertAll(Calc.Halves(5), x => ((int)(x * 10)).ToString())));
		P("Churn", Calc.Churn("hello", new int[] { 1, 2, 3 }, 3000));
		P("Churn again", Calc.Churn("a longer string than before", new int[] { 10, 20 }, 3000));

		// objects
		Counter c = Calc.NewCounter(10);
		P("Obj bump 1", Calc.Bump(c, 5));
		P("Obj bump 2", Calc.Bump(c, 7));
		Counter d = Calc.Same(c);
		P("Obj same", Calc.Bump(d, 1));
		P("Obj after drop", Calc.Bump(d, 1));
		P("Obj total", Calc.CTotal(d));
		P("Obj tag 1", Calc.CTag(d, "x"));
		P("Obj tag 2", Calc.CTag(c, "y"));
		P("Obj null", Calc.IsNull(null) ? 1 : 0);
		P("Obj not null", Calc.IsNull(c) ? 1 : 0);
		Counter[] many = new Counter[200];
		for (int i = 0; i < many.Length; i++) { many[i] = Calc.NewCounter(i); Calc.Bump(many[i], i & 7); }
		P("Obj churn", Calc.Churn("held", new int[] { 1, 2, 3 }, 6000));
		long okHist = 0, okTotal = 0;
		for (int i = 0; i < many.Length; i++) {
			okHist += Calc.CHist(many[i], i & 7);
			okTotal += Calc.CTotal(many[i]) - (i + (i & 7));
		}
		P("Obj children", okHist);
		P("Obj totals", okTotal);
	}
}
