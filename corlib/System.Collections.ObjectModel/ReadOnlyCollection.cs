#if !LOCALTEST

using System.Collections.Generic;

namespace System.Collections.ObjectModel {
	/// <summary>A read-only view of a list (changes to the list show through).</summary>
	public class ReadOnlyCollection<T> : IList<T>, IEnumerable<T> {
		readonly IList<T> list;

		public ReadOnlyCollection(IList<T> list) {
			if (list == null) throw new ArgumentNullException("list");
			this.list = list;
		}

		public int Count { get { return list.Count; } }
		public T this[int index] { get { return list[index]; } set { throw new NotSupportedException("Collection is read-only."); } }
		public bool IsReadOnly { get { return true; } }
		public bool Contains(T value) { return list.Contains(value); }
		public int IndexOf(T value) { return list.IndexOf(value); }
		public void CopyTo(T[] array, int index) { list.CopyTo(array, index); }
		public IEnumerator<T> GetEnumerator() { return list.GetEnumerator(); }
		IEnumerator IEnumerable.GetEnumerator() { return list.GetEnumerator(); }
		void ICollection<T>.Add(T item) { throw new NotSupportedException("Collection is read-only."); }
		void ICollection<T>.Clear() { throw new NotSupportedException("Collection is read-only."); }
		bool ICollection<T>.Remove(T item) { throw new NotSupportedException("Collection is read-only."); }
		void IList<T>.Insert(int index, T item) { throw new NotSupportedException("Collection is read-only."); }
		void IList<T>.RemoveAt(int index) { throw new NotSupportedException("Collection is read-only."); }
	}
}

#endif
