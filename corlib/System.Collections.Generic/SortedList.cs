#if !LOCALTEST

namespace System.Collections.Generic {
	/// <summary>Pairs kept in key order (two parallel lists, found by binary search).</summary>
	public class SortedList<TKey, TValue> : IEnumerable<KeyValuePair<TKey, TValue>>, IEnumerable {
		readonly List<TKey> keys = new List<TKey>();
		readonly List<TValue> values = new List<TValue>();
		readonly IComparer<TKey> comparer;

		public SortedList() : this((IComparer<TKey>)null) { }
		public SortedList(int capacity) : this((IComparer<TKey>)null) { }
		public SortedList(IComparer<TKey> comparer) { this.comparer = comparer ?? Comparer<TKey>.Default; }
		public SortedList(IDictionary<TKey, TValue> dictionary) : this((IComparer<TKey>)null) {
			foreach (KeyValuePair<TKey, TValue> kv in dictionary) Add(kv.Key, kv.Value);
		}

		public int Count { get { return keys.Count; } }
		public IComparer<TKey> Comparer { get { return comparer; } }
		// ponytail: the live lists, so a caller could unsort them (.NET hands out read-only views).  Upgrade path: ReadOnlyCollection wrappers.
		public IList<TKey> Keys { get { return keys; } }
		public IList<TValue> Values { get { return values; } }

		/// <summary>The key's index, else the bitwise complement of where it would go.</summary>
		int Search(TKey key) {
			if (key == null) throw new ArgumentNullException("key");
			int lo = 0, hi = keys.Count - 1;
			while (lo <= hi) {
				int mid = (lo + hi) >> 1, c = comparer.Compare(keys[mid], key);
				if (c == 0) return mid;
				if (c < 0) lo = mid + 1; else hi = mid - 1;
			}
			return ~lo;
		}

		public TValue this[TKey key] {
			get {
				int i = Search(key);
				if (i < 0) throw new KeyNotFoundException();
				return values[i];
			}
			set {
				int i = Search(key);
				if (i >= 0) { values[i] = value; return; }
				keys.Insert(~i, key);
				values.Insert(~i, value);
			}
		}

		public void Add(TKey key, TValue value) {
			int i = Search(key);
			if (i >= 0) throw new ArgumentException("An entry with the same key already exists.");
			keys.Insert(~i, key);
			values.Insert(~i, value);
		}

		public bool Remove(TKey key) {
			int i = Search(key);
			if (i < 0) return false;
			RemoveAt(i);
			return true;
		}

		public void RemoveAt(int index) {
			keys.RemoveAt(index);
			values.RemoveAt(index);
		}

		public void Clear() { keys.Clear(); values.Clear(); }
		public bool ContainsKey(TKey key) { return Search(key) >= 0; }
		public bool ContainsValue(TValue value) { return values.IndexOf(value) >= 0; }
		public int IndexOfKey(TKey key) { int i = Search(key); return i < 0 ? -1 : i; }
		public int IndexOfValue(TValue value) { return values.IndexOf(value); }
		public TKey GetKeyAtIndex(int index) { return keys[index]; }
		public TValue GetValueAtIndex(int index) { return values[index]; }

		public bool TryGetValue(TKey key, out TValue value) {
			int i = Search(key);
			value = i < 0 ? default(TValue) : values[i];
			return i >= 0;
		}

		public IEnumerator<KeyValuePair<TKey, TValue>> GetEnumerator() {
			for (int i = 0; i < keys.Count; i++) yield return new KeyValuePair<TKey, TValue>(keys[i], values[i]);
		}

		IEnumerator IEnumerable.GetEnumerator() { return GetEnumerator(); }
	}
}

#endif
