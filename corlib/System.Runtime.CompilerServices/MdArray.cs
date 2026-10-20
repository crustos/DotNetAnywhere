#if !LOCALTEST

using System.Collections;
using System.Collections.Generic;

namespace System.Runtime.CompilerServices {
	// DNA has no multi-dimensional arrays (ELEMENT_TYPE_ARRAY): CC# --dna compiles `T[,]` and `T[,,]` as these (row-major, as .NET lays them out).
	// ponytail: what takes a System.Array (Array.Copy, Array.Clear, a cast to Array) does not take one of these.  Upgrade path: rank > 1 arrays in
	// DNA's runtime (an array type per rank, its .ctor / Get / Set / Address made by the JIT).
	public sealed class MdArray2<T> : IEnumerable<T> {
		readonly T[] items;
		readonly int n0, n1;

		public MdArray2(int length0, int length1) {
			if (length0 < 0 || length1 < 0) throw new OverflowException("Arithmetic operation resulted in an overflow.");
			n0 = length0;
			n1 = length1;
			items = new T[length0 * length1];
		}

		/// <summary>`new T[,] { { a, b }, { c, d } }`: the rows, each as long as the first.</summary>
		public static MdArray2<T> Of(T[][] rows) {
			MdArray2<T> a = new MdArray2<T>(rows.Length, rows.Length == 0 ? 0 : rows[0].Length);
			for (int i = 0; i < rows.Length; i++) {
				if (rows[i].Length != a.n1) throw new ArgumentException("An array initializer of length " + a.n1 + " is expected.");
				for (int j = 0; j < a.n1; j++) a.items[i * a.n1 + j] = rows[i][j];
			}
			return a;
		}

		int At(int i, int j) {
			if ((uint)i >= (uint)n0 || (uint)j >= (uint)n1) throw new IndexOutOfRangeException("Index was outside the bounds of the array.");
			return i * n1 + j;
		}

		public T this[int i, int j] { get { return items[At(i, j)]; } set { items[At(i, j)] = value; } }
		public int Length { get { return items.Length; } }
		public long LongLength { get { return items.Length; } }
		public int Rank { get { return 2; } }
		public int GetLength(int dimension) {
			if (dimension == 0) return n0;
			if (dimension == 1) return n1;
			throw new IndexOutOfRangeException("Index was outside the bounds of the array.");
		}
		public int GetLowerBound(int dimension) { GetLength(dimension); return 0; }
		public int GetUpperBound(int dimension) { return GetLength(dimension) - 1; }
		public object Clone() { MdArray2<T> c = new MdArray2<T>(n0, n1); Array.Copy(items, c.items, items.Length); return c; }
		public IEnumerator<T> GetEnumerator() { for (int i = 0; i < items.Length; i++) yield return items[i]; }
		IEnumerator IEnumerable.GetEnumerator() { return GetEnumerator(); }
	}

	public sealed class MdArray3<T> : IEnumerable<T> {
		readonly T[] items;
		readonly int n0, n1, n2;

		public MdArray3(int length0, int length1, int length2) {
			if (length0 < 0 || length1 < 0 || length2 < 0) throw new OverflowException("Arithmetic operation resulted in an overflow.");
			n0 = length0;
			n1 = length1;
			n2 = length2;
			items = new T[length0 * length1 * length2];
		}

		int At(int i, int j, int k) {
			if ((uint)i >= (uint)n0 || (uint)j >= (uint)n1 || (uint)k >= (uint)n2) throw new IndexOutOfRangeException("Index was outside the bounds of the array.");
			return (i * n1 + j) * n2 + k;
		}

		public T this[int i, int j, int k] { get { return items[At(i, j, k)]; } set { items[At(i, j, k)] = value; } }
		public int Length { get { return items.Length; } }
		public long LongLength { get { return items.Length; } }
		public int Rank { get { return 3; } }
		public int GetLength(int dimension) {
			if (dimension == 0) return n0;
			if (dimension == 1) return n1;
			if (dimension == 2) return n2;
			throw new IndexOutOfRangeException("Index was outside the bounds of the array.");
		}
		public int GetLowerBound(int dimension) { GetLength(dimension); return 0; }
		public int GetUpperBound(int dimension) { return GetLength(dimension) - 1; }
		public object Clone() { MdArray3<T> c = new MdArray3<T>(n0, n1, n2); Array.Copy(items, c.items, items.Length); return c; }
		public IEnumerator<T> GetEnumerator() { for (int i = 0; i < items.Length; i++) yield return items[i]; }
		IEnumerator IEnumerable.GetEnumerator() { return GetEnumerator(); }
	}
}

#endif
