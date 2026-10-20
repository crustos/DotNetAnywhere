#if !LOCALTEST

namespace System.Text.RegularExpressions {
	// (no Regex engine: the options exist for programs that keep them as data)
	[Flags]
	public enum RegexOptions {
		None = 0, IgnoreCase = 1, Multiline = 2, ExplicitCapture = 4, Compiled = 8, Singleline = 16, IgnorePatternWhitespace = 32,
		RightToLeft = 64, ECMAScript = 256, CultureInvariant = 512, NonBacktracking = 1024,
	}
}

#endif
