#if !LOCALTEST

namespace System.Collections.Generic {

	/// <summary>A last-in first-out collection: enumerated from the top, as .NET's is.</summary>
	public class Stack<T> : IEnumerable<T>, ICollection {

		T[] items;
		int count;

		public Stack() { items = new T[4]; }

		public Stack(int capacity) { items = new T[capacity < 1 ? 1 : capacity]; }

		public Stack(IEnumerable<T> collection) : this() {
			foreach (T item in collection) Push(item);
		}

		public int Count { get { return count; } }

		public void Push(T item) {
			if (count == items.Length) {
				T[] bigger = new T[items.Length * 2];
				Array.Copy(items, bigger, count);
				items = bigger;
			}
			items[count++] = item;
		}

		public T Pop() {
			if (count == 0) throw new InvalidOperationException("Stack empty.");
			T item = items[--count];
			items[count] = default(T);
			return item;
		}

		public T Peek() {
			if (count == 0) throw new InvalidOperationException("Stack empty.");
			return items[count - 1];
		}

		public bool TryPop(out T result) {
			if (count == 0) { result = default(T); return false; }
			result = Pop();
			return true;
		}

		public bool TryPeek(out T result) {
			if (count == 0) { result = default(T); return false; }
			result = items[count - 1];
			return true;
		}

		public void Clear() {
			for (int i = 0; i < count; i++) items[i] = default(T);
			count = 0;
		}

		public bool Contains(T item) {
			EqualityComparer<T> c = EqualityComparer<T>.Default;
			for (int i = 0; i < count; i++) if (c.Equals(items[i], item)) return true;
			return false;
		}

		public T[] ToArray() {
			T[] a = new T[count];
			for (int i = 0; i < count; i++) a[i] = items[count - 1 - i];
			return a;
		}

		public IEnumerator<T> GetEnumerator() {
			for (int i = count - 1; i >= 0; i--) yield return items[i];
		}

		IEnumerator IEnumerable.GetEnumerator() { return GetEnumerator(); }

		bool ICollection.IsSynchronized { get { return false; } }
		object ICollection.SyncRoot { get { return this; } }
		void ICollection.CopyTo(Array array, int index) {
			for (int i = 0; i < count; i++) array.SetValue(items[count - 1 - i], index + i);
		}
	}
}

#endif
