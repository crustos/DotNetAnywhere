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

using System;

#if LOCALTEST
using System.Collections;
using System.Collections.Generic;
namespace System_.Collections.Generic {
#else
namespace System.Collections.Generic {
#endif
	public class List<T> : IList<T>, ICollection<T>, IEnumerable<T>, IList, ICollection, IEnumerable {

		public struct Enumerator : IEnumerator<T>, IDisposable {

			private List<T> list;
			private int index;

			internal Enumerator(List<T> list) {
				this.list = list;
				this.index = -1;
			}

			public T Current {
				get {
					return this.list[this.index];
				}
			}

			public void Dispose() {
			}

			object IEnumerator.Current {
				get {
					return this.list[this.index];
				}
			}

			public bool MoveNext() {
				this.index++;
				return (this.index < this.list.Count);
			}

			public void Reset() {
				this.index = -1;
			}

		}

		private const int defaultCapacity = 4;

		private T[] items;
		private int size;

		public List() : this(defaultCapacity) { }

		public List(int capacity) {
			if (capacity < 0) {
				throw new ArgumentOutOfRangeException("capacity");
			}
			this.items = new T[capacity];
			this.size = 0;
		}

		public List(IEnumerable<T> collection) {
			ICollection<T> iCol = collection as ICollection<T>;
			if (iCol != null) {
				this.size = iCol.Count;
				this.items = new T[this.size];
				iCol.CopyTo(this.items, 0);
			} else {
				this.items = new T[defaultCapacity];
				this.size = 0;
				foreach (T item in collection) {
					this.Add(item);
				}
			}
		}

		private void EnsureSpace(int space) {
			if (this.size + space > this.items.Length) {
				Array.Resize<T>(ref this.items, Math.Max(this.items.Length << 1, 4));
			}
		}

		private void Shift(int index, int count) {
			if (count > 0) {
				this.EnsureSpace(count);
				for (int i = this.size - 1; i >= index; i--) {
					this.items[i + count] = this.items[i];
				}
			} else {
				for (int i = index; i < this.size + count; i++) {
					this.items[i] = this.items[i - count];
				}

                // [Steve note] The above logic isn't enough to release the remaining
                // items for GC, e.g., when shifting the trailing items.
                // So, clear out the now-unused trailing slots.
                for (int i = this.size - 1; i >= this.size + count; i--)
                {
                    this.items[i] = default(T);
                }
			}
			this.size += count;
		}

		public void Add(T item) {
			this.EnsureSpace(1);
			this.items[this.size++] = item;
		}

		public int Count {
			get {
				return this.size;
			}
		}

		public int Capacity {
			get {
				return this.items.Length;
			}
			set {
				throw new NotImplementedException();
			}
		}

		public T this[int index] {
			get {
				if (index >= this.size || index < 0) {
					throw new ArgumentOutOfRangeException("index");
				}
				return this.items[index];
			}
			set {
				if (index >= this.size || index < 0) {
					throw new ArgumentOutOfRangeException("index");
				}
				this.items[index] = value;
			}
		}

		public Enumerator GetEnumerator() {
			return new Enumerator(this);
		}

		public int IndexOf(T item, int start, int count) {
			return Array.IndexOf<T>(this.items, item, start, count);
		}

		public int IndexOf(T item, int start) {
			return this.IndexOf(item, start, this.size - start);
		}

		public void InsertRange(int index, IEnumerable<T> collection) {
			if (collection == null) {
				throw new ArgumentNullException("collection");
			}
			if (index < 0 || index > this.size) {
				throw new ArgumentOutOfRangeException("index");
			}
			List<T> toInsert = new List<T>(collection);
			this.Shift(index, toInsert.Count);
			for (int i = 0; i < toInsert.Count; i++) {
				this.items[index + i] = toInsert[i];
			}
		}

		public void AddRange(IEnumerable<T> collection) {
			this.InsertRange(this.size, collection);
		}

		public void RemoveRange(int index, int count) {
			if (index < 0 || count < 0 || index + count > this.size) {
				throw new ArgumentOutOfRangeException(index < 0 ? "index" : "count");
			}
			this.Shift(index, -count);
		}

		public T[] ToArray() {
			T[] array = new T[this.size];
			Array.Copy(this.items, array, this.size);
			return array;
		}

		public void Sort() { Sort(0, this.size, null); }
		public void Sort(IComparer<T> comparer) { Sort(0, this.size, comparer); }
		public void Sort(int index, int count, IComparer<T> comparer) {
			if (index < 0 || count < 0 || index + count > this.size) throw new ArgumentOutOfRangeException();
			Array.Sort(this.items, index, count, comparer);
		}
		public void Sort(Comparison<T> comparison) {
			if (comparison == null) throw new ArgumentNullException("comparison");
			Array.MergeSort(this.items, 0, this.size, comparison);
		}

		public void Reverse() { Reverse(0, this.size); }
		public void Reverse(int index, int count) {
			if (index < 0 || count < 0 || index + count > this.size) throw new ArgumentOutOfRangeException();
			for (int i = index, j = index + count - 1; i < j; i++, j--) { T t = this.items[i]; this.items[i] = this.items[j]; this.items[j] = t; }
		}

		public T Find(Predicate<T> match) {
			for (int i = 0; i < this.size; i++) if (match(this.items[i])) return this.items[i];
			return default(T);
		}
		public T FindLast(Predicate<T> match) {
			for (int i = this.size - 1; i >= 0; i--) if (match(this.items[i])) return this.items[i];
			return default(T);
		}
		public List<T> FindAll(Predicate<T> match) {
			List<T> r = new List<T>();
			for (int i = 0; i < this.size; i++) if (match(this.items[i])) r.Add(this.items[i]);
			return r;
		}
		public int FindIndex(Predicate<T> match) { return FindIndex(0, this.size, match); }
		public int FindIndex(int startIndex, Predicate<T> match) { return FindIndex(startIndex, this.size - startIndex, match); }
		public int FindIndex(int startIndex, int count, Predicate<T> match) {
			if (startIndex < 0 || count < 0 || startIndex + count > this.size) throw new ArgumentOutOfRangeException();
			for (int i = startIndex; i < startIndex + count; i++) if (match(this.items[i])) return i;
			return -1;
		}
		public int FindLastIndex(Predicate<T> match) {
			for (int i = this.size - 1; i >= 0; i--) if (match(this.items[i])) return i;
			return -1;
		}
		public bool Exists(Predicate<T> match) { return FindIndex(match) >= 0; }
		public bool TrueForAll(Predicate<T> match) {
			for (int i = 0; i < this.size; i++) if (!match(this.items[i])) return false;
			return true;
		}
		public int RemoveAll(Predicate<T> match) {
			int k = 0;
			for (int i = 0; i < this.size; i++) if (!match(this.items[i])) this.items[k++] = this.items[i];
			int removed = this.size - k;
			for (int i = k; i < this.size; i++) this.items[i] = default(T);
			this.size = k;
			return removed;
		}
		public void ForEach(Action<T> action) {
			for (int i = 0; i < this.size; i++) action(this.items[i]);
		}
		public List<T> GetRange(int index, int count) {
			if (index < 0 || count < 0 || index + count > this.size) throw new ArgumentOutOfRangeException();
			List<T> r = new List<T>(count);
			for (int i = index; i < index + count; i++) r.Add(this.items[i]);
			return r;
		}
		public List<TOutput> ConvertAll<TOutput>(Converter<T, TOutput> converter) {
			List<TOutput> r = new List<TOutput>(this.size);
			for (int i = 0; i < this.size; i++) r.Add(converter(this.items[i]));
			return r;
		}
		public int LastIndexOf(T item) {
			EqualityComparer<T> eq = EqualityComparer<T>.Default;
			for (int i = this.size - 1; i >= 0; i--) if (eq.Equals(this.items[i], item)) return i;
			return -1;
		}

		#region Interface Members

		public int IndexOf(T item) {
			return this.IndexOf(item, 0, size);
		}

		public void Insert(int index, T item) {
			if (index < 0 || index > this.size) {
				throw new ArgumentOutOfRangeException("index");
			}
			this.Shift(index, 1);
			this.items[index] = item;
		}

		public void RemoveAt(int index) {
			if (index < 0 || index >= this.size) {
				throw new ArgumentOutOfRangeException("index");
			}
			this.Shift(index, -1);
		}

		public bool IsReadOnly {
			get {
				return false;
			}
		}

		public void Clear() {
			Array.Clear(this.items, 0, this.items.Length);
			this.size = 0;
		}

		public bool Contains(T item) {
			return Array.IndexOf(this.items, item) >= 0;
		}

		public void CopyTo(T[] array) { CopyTo(array, 0); }

		public void CopyTo(T[] array, int arrayIndex) {
			Array.Copy(this.items, 0, (Array)array, arrayIndex, this.size);
		}

		public bool Remove(T item) {
			// Search only the live elements. Searching the whole backing array also matched
			// the default(T) slots past `size`, so Remove(default) on a list lacking that
			// value reported success and deleted the last element.
			int idx = this.IndexOf(item);
			if (idx >= 0) {
				this.RemoveAt(idx);
				return true;
			}
			return false;
		}

		IEnumerator<T> IEnumerable<T>.GetEnumerator() {
			return new Enumerator(this);
		}

		IEnumerator IEnumerable.GetEnumerator() {
			return new Enumerator(this);
		}

		public bool IsFixedSize {
			get {
				return false;
			}
		}

		object IList.this[int index] {
			get {
				return this[index];
			}
			set {
				this[index] = (T)value;
			}
		}

		public int Add(object value) {
			this.Add((T)value);
			return this.items.Length - 1;
		}

		public bool Contains(object value) {
			return this.Contains((T)value);
		}

		public int IndexOf(object value) {
			return this.IndexOf((T)value);
		}

		public void Insert(int index, object value) {
			this.Insert(index, (T)value);
		}

		public void Remove(object value) {
			this.Remove((T)value);
		}

		public bool IsSynchronized {
			get {
				return false;
			}
		}

		public object SyncRoot {
			get {
				return this;
			}
		}

		public void CopyTo(Array array, int index) {
			Array.Copy(this.items, 0, array, index, this.size);
		}

		#endregion
	}
}
