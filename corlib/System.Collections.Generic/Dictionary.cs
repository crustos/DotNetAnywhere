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

	// Table sizes for the dictionary: primes, so that hash codes spread over the buckets.
	internal static class DictionaryPrimes {
		private static readonly int[] primes = {
			3, 7, 11, 17, 23, 29, 37, 47, 59, 71, 89, 107, 131, 163, 197, 239, 293, 353, 431, 521, 631, 761, 919,
			1103, 1327, 1597, 1931, 2333, 2801, 3371, 4049, 4861, 5839, 7013, 8419, 10103, 12143, 14591, 17519,
			21023, 25229, 30293, 36353, 43627, 52361, 62851, 75431, 90523, 108631, 130363, 156437, 187751, 225307,
			270371, 324449, 389357, 467237, 560689, 672827, 807403, 968897, 1162687, 1395263, 1674319, 2009191,
			2411033, 2893249, 3471899, 4166287, 4999559, 5999471, 7199369
		};

		private static bool IsPrime(int n) {
			if ((n & 1) == 0) {
				return n == 2;
			}
			int limit = (int)Math.Sqrt(n);
			for (int d = 3; d <= limit; d += 2) {
				if (n % d == 0) {
					return false;
				}
			}
			return true;
		}

		// The smallest prime that is at least `min`
		public static int GetPrime(int min) {
			for (int i = 0; i < primes.Length; i++) {
				if (primes[i] >= min) {
					return primes[i];
				}
			}
			for (int c = min | 1; c < int.MaxValue; c += 2) {
				if (IsPrime(c)) {
					return c;
				}
			}
			return min;
		}
	}

	// Chained hash table in two arrays, so adding an item allocates nothing (it used to allocate two Lists per slot,
	// and was slow and wrong: Count was one too low after the first resize). buckets[h] holds the index + 1 of the
	// first entry whose hash falls in bucket h (0 = none); entries are linked through `next`. Entries are appended in
	// order, and a removed entry goes on a free list that the next Add reuses, so enumeration order is the insertion
	// order (as in Mono and .NET) until something is removed.
	public class Dictionary<TKey, TValue> : IDictionary<TKey, TValue>, ICollection<KeyValuePair<TKey, TValue>>,
		IEnumerable<KeyValuePair<TKey, TValue>>, IDictionary, ICollection, IEnumerable {

		private struct Entry {
			public int hashCode;    // -1 when the entry is free
			public int next;        // next entry in this bucket's chain (or in the free list); -1 ends it
			public TKey key;
			public TValue value;
		}

		public struct Enumerator : IEnumerator<KeyValuePair<TKey, TValue>>,
			IDisposable, IDictionaryEnumerator, IEnumerator {

			private Dictionary<TKey, TValue> dict;
			private int version;
			private int index;
			private KeyValuePair<TKey, TValue> current;

			internal Enumerator(Dictionary<TKey, TValue> dictionary) {
				this.dict = dictionary;
				this.version = dictionary.version;
				this.index = 0;
				this.current = default(KeyValuePair<TKey, TValue>);
			}

			public bool MoveNext() {
				if (this.version != this.dict.version) {
					throw new InvalidOperationException("Collection was modified; enumeration operation may not be executed.");
				}
				while (this.index < this.dict.count) {
					int i = this.index++;
					if (this.dict.entries[i].hashCode >= 0) {
						this.current = new KeyValuePair<TKey, TValue>(this.dict.entries[i].key, this.dict.entries[i].value);
						return true;
					}
				}
				this.index = this.dict.count + 1;
				this.current = default(KeyValuePair<TKey, TValue>);
				return false;
			}

			public KeyValuePair<TKey, TValue> Current {
				get { return this.current; }
			}

			private void CheckPosition() {
				if (this.index == 0 || this.index == this.dict.count + 1) {
					throw new InvalidOperationException("Enumeration has either not started or has already finished.");
				}
			}

			object IEnumerator.Current {
				get { return this.current; }
			}

			void IEnumerator.Reset() {
				if (this.version != this.dict.version) {
					throw new InvalidOperationException("Collection was modified; enumeration operation may not be executed.");
				}
				this.index = 0;
				this.current = default(KeyValuePair<TKey, TValue>);
			}

			DictionaryEntry IDictionaryEnumerator.Entry {
				get {
					this.CheckPosition();
					return new DictionaryEntry(this.current.Key, this.current.Value);
				}
			}

			object IDictionaryEnumerator.Key {
				get {
					this.CheckPosition();
					return this.current.Key;
				}
			}

			object IDictionaryEnumerator.Value {
				get {
					this.CheckPosition();
					return this.current.Value;
				}
			}

			public void Dispose() {
			}
		}

		public sealed class KeyCollection : ICollection<TKey>, IEnumerable<TKey>, ICollection, IEnumerable {

			public struct Enumerator : IEnumerator<TKey>, IDisposable, IEnumerator {

				private Dictionary<TKey, TValue>.Enumerator hostEnumerator;

				internal Enumerator(Dictionary<TKey, TValue> host) {
					this.hostEnumerator = host.GetEnumerator();
				}

				public void Dispose() {
					this.hostEnumerator.Dispose();
				}

				public bool MoveNext() {
					return this.hostEnumerator.MoveNext();
				}

				public TKey Current {
					get { return this.hostEnumerator.Current.Key; }
				}

				object IEnumerator.Current {
					get { return this.hostEnumerator.Current.Key; }
				}

				void IEnumerator.Reset() {
					((IEnumerator)this.hostEnumerator).Reset();
				}
			}

			private Dictionary<TKey, TValue> dictionary;

			public KeyCollection(Dictionary<TKey, TValue> dictionary) {
				if (dictionary == null) {
					throw new ArgumentNullException("dictionary");
				}
				this.dictionary = dictionary;
			}

			public void CopyTo(TKey[] array, int index) {
				this.dictionary.CheckCopyTo(array == null, array == null ? 0 : array.Length, index);
				Dictionary<TKey, TValue> d = this.dictionary;
				for (int i = 0; i < d.count; i++) {
					if (d.entries[i].hashCode >= 0) {
						array[index++] = d.entries[i].key;
					}
				}
			}

			public Enumerator GetEnumerator() {
				return new Enumerator(this.dictionary);
			}

			void ICollection<TKey>.Add(TKey item) {
				throw new NotSupportedException("this is a read-only collection");
			}

			void ICollection<TKey>.Clear() {
				throw new NotSupportedException("this is a read-only collection");
			}

			bool ICollection<TKey>.Contains(TKey item) {
				return this.dictionary.ContainsKey(item);
			}

			bool ICollection<TKey>.Remove(TKey item) {
				throw new NotSupportedException("this is a read-only collection");
			}

			IEnumerator<TKey> IEnumerable<TKey>.GetEnumerator() {
				return this.GetEnumerator();
			}

			void ICollection.CopyTo(Array array, int index) {
				this.CopyTo((TKey[])array, index);
			}

			IEnumerator IEnumerable.GetEnumerator() {
				return this.GetEnumerator();
			}

			public int Count {
				get { return this.dictionary.Count; }
			}

			bool ICollection<TKey>.IsReadOnly {
				get { return true; }
			}

			bool ICollection.IsSynchronized {
				get { return false; }
			}

			object ICollection.SyncRoot {
				get { return ((ICollection)this.dictionary).SyncRoot; }
			}
		}

		public sealed class ValueCollection : ICollection<TValue>, IEnumerable<TValue>, ICollection, IEnumerable {

			public struct Enumerator : IEnumerator<TValue>, IDisposable, IEnumerator {

				private Dictionary<TKey, TValue>.Enumerator hostEnumerator;

				internal Enumerator(Dictionary<TKey, TValue> host) {
					this.hostEnumerator = host.GetEnumerator();
				}

				public void Dispose() {
					this.hostEnumerator.Dispose();
				}

				public bool MoveNext() {
					return this.hostEnumerator.MoveNext();
				}

				public TValue Current {
					get { return this.hostEnumerator.Current.Value; }
				}

				object IEnumerator.Current {
					get { return this.hostEnumerator.Current.Value; }
				}

				void IEnumerator.Reset() {
					((IEnumerator)this.hostEnumerator).Reset();
				}
			}

			private Dictionary<TKey, TValue> dictionary;

			public ValueCollection(Dictionary<TKey, TValue> dictionary) {
				if (dictionary == null) {
					throw new ArgumentNullException("dictionary");
				}
				this.dictionary = dictionary;
			}

			public void CopyTo(TValue[] array, int index) {
				this.dictionary.CheckCopyTo(array == null, array == null ? 0 : array.Length, index);
				Dictionary<TKey, TValue> d = this.dictionary;
				for (int i = 0; i < d.count; i++) {
					if (d.entries[i].hashCode >= 0) {
						array[index++] = d.entries[i].value;
					}
				}
			}

			public Enumerator GetEnumerator() {
				return new Enumerator(this.dictionary);
			}

			void ICollection<TValue>.Add(TValue item) {
				throw new NotSupportedException("this is a read-only collection");
			}

			void ICollection<TValue>.Clear() {
				throw new NotSupportedException("this is a read-only collection");
			}

			bool ICollection<TValue>.Contains(TValue item) {
				return this.dictionary.ContainsValue(item);
			}

			bool ICollection<TValue>.Remove(TValue item) {
				throw new NotSupportedException("this is a read-only collection");
			}

			IEnumerator<TValue> IEnumerable<TValue>.GetEnumerator() {
				return this.GetEnumerator();
			}

			void ICollection.CopyTo(Array array, int index) {
				this.CopyTo((TValue[])array, index);
			}

			IEnumerator IEnumerable.GetEnumerator() {
				return this.GetEnumerator();
			}

			public int Count {
				get { return this.dictionary.Count; }
			}

			bool ICollection<TValue>.IsReadOnly {
				get { return true; }
			}

			bool ICollection.IsSynchronized {
				get { return false; }
			}

			object ICollection.SyncRoot {
				get { return ((ICollection)this.dictionary).SyncRoot; }
			}
		}

		private int[] buckets;
		private Entry[] entries;
		private int count;          // entries used so far, including ones that have since been removed
		private int freeList;       // the first removed entry (-1 if none); they are chained through `next`
		private int freeCount;      // how many removed entries are on the free list
		private int version;        // changed by every modification, so enumerators can notice
		private IEqualityComparer<TKey> comparer;

		public Dictionary() {
			this.Init(0, null);
		}

		public Dictionary(IDictionary<TKey, TValue> dictionary) : this(dictionary, null) { }

		public Dictionary(IEqualityComparer<TKey> comparer) {
			this.Init(0, comparer);
		}

		public Dictionary(int capacity) {
			this.Init(capacity, null);
		}

		public Dictionary(IDictionary<TKey, TValue> dictionary, IEqualityComparer<TKey> comparer) {
			if (dictionary == null) {
				throw new ArgumentNullException("dictionary");
			}
			this.Init(dictionary.Count, comparer);
			foreach (KeyValuePair<TKey, TValue> item in dictionary) {
				this.Add(item.Key, item.Value);
			}
		}

		public Dictionary(int capacity, IEqualityComparer<TKey> comparer) {
			this.Init(capacity, comparer);
		}

		private void Init(int capacity, IEqualityComparer<TKey> comparer) {
			if (capacity < 0) {
				throw new ArgumentOutOfRangeException("capacity");
			}
			this.comparer = comparer ?? EqualityComparer<TKey>.Default;
			this.freeList = -1;
			if (capacity > 0) {
				this.Initialize(capacity);
			}
		}

		private void Initialize(int capacity) {
			int size = DictionaryPrimes.GetPrime(capacity);
			this.buckets = new int[size];
			this.entries = new Entry[size];
			this.freeList = -1;
		}

		// Where `key` is in entries, or -1
		private int FindEntry(TKey key) {
			if (key == null) {
				throw new ArgumentNullException("key");
			}
			if (this.buckets != null) {
				int hashCode = this.comparer.GetHashCode(key) & 0x7fffffff;
				for (int i = this.buckets[hashCode % this.buckets.Length] - 1; i >= 0; i = this.entries[i].next) {
					if (this.entries[i].hashCode == hashCode && this.comparer.Equals(this.entries[i].key, key)) {
						return i;
					}
				}
			}
			return -1;
		}

		private void Insert(TKey key, TValue value, bool add) {
			if (key == null) {
				throw new ArgumentNullException("key");
			}
			if (this.buckets == null) {
				this.Initialize(0);
			}
			int hashCode = this.comparer.GetHashCode(key) & 0x7fffffff;
			int targetBucket = hashCode % this.buckets.Length;
			for (int i = this.buckets[targetBucket] - 1; i >= 0; i = this.entries[i].next) {
				if (this.entries[i].hashCode == hashCode && this.comparer.Equals(this.entries[i].key, key)) {
					if (add) {
						throw new ArgumentException("An item with the same key has already been added.");
					}
					this.entries[i].value = value;
					this.version++;
					return;
				}
			}
			int index;
			if (this.freeCount > 0) {
				index = this.freeList;
				this.freeList = this.entries[index].next;
				this.freeCount--;
			} else {
				if (this.count == this.entries.Length) {
					this.Resize();
					targetBucket = hashCode % this.buckets.Length;
				}
				index = this.count;
				this.count++;
			}
			this.entries[index].hashCode = hashCode;
			this.entries[index].next = this.buckets[targetBucket] - 1;
			this.entries[index].key = key;
			this.entries[index].value = value;
			this.buckets[targetBucket] = index + 1;
			this.version++;
		}

		// Twice the size, rehashing every entry into the new buckets. The entries keep their positions, so the
		// enumeration order does not change.
		private void Resize() {
			int newSize = DictionaryPrimes.GetPrime(this.count * 2);
			int[] newBuckets = new int[newSize];
			Entry[] newEntries = new Entry[newSize];
			Array.Copy(this.entries, 0, newEntries, 0, this.count);
			for (int i = 0; i < this.count; i++) {
				if (newEntries[i].hashCode >= 0) {
					int bucket = newEntries[i].hashCode % newSize;
					newEntries[i].next = newBuckets[bucket] - 1;
					newBuckets[bucket] = i + 1;
				}
			}
			this.buckets = newBuckets;
			this.entries = newEntries;
		}

		public void Add(TKey key, TValue value) {
			this.Insert(key, value, true);
		}

		public TValue this[TKey key] {
			get {
				int i = this.FindEntry(key);
				if (i >= 0) {
					return this.entries[i].value;
				}
				throw new KeyNotFoundException("The given key was not present in the dictionary.");
			}
			set {
				this.Insert(key, value, false);
			}
		}

		public bool TryGetValue(TKey key, out TValue value) {
			int i = this.FindEntry(key);
			if (i >= 0) {
				value = this.entries[i].value;
				return true;
			}
			value = default(TValue);
			return false;
		}

		public bool ContainsKey(TKey key) {
			return this.FindEntry(key) >= 0;
		}

		public bool ContainsValue(TValue value) {
			if (value == null) {
				for (int i = 0; i < this.count; i++) {
					if (this.entries[i].hashCode >= 0 && this.entries[i].value == null) {
						return true;
					}
				}
			} else {
				EqualityComparer<TValue> valueComparer = EqualityComparer<TValue>.Default;
				for (int i = 0; i < this.count; i++) {
					if (this.entries[i].hashCode >= 0 && valueComparer.Equals(this.entries[i].value, value)) {
						return true;
					}
				}
			}
			return false;
		}

		public bool Remove(TKey key) {
			if (key == null) {
				throw new ArgumentNullException("key");
			}
			if (this.buckets != null) {
				int hashCode = this.comparer.GetHashCode(key) & 0x7fffffff;
				int bucket = hashCode % this.buckets.Length;
				int last = -1;
				for (int i = this.buckets[bucket] - 1; i >= 0; last = i, i = this.entries[i].next) {
					if (this.entries[i].hashCode == hashCode && this.comparer.Equals(this.entries[i].key, key)) {
						if (last < 0) {
							this.buckets[bucket] = this.entries[i].next + 1;
						} else {
							this.entries[last].next = this.entries[i].next;
						}
						this.entries[i].hashCode = -1;
						this.entries[i].next = this.freeList;
						this.entries[i].key = default(TKey);
						this.entries[i].value = default(TValue);
						this.freeList = i;
						this.freeCount++;
						this.version++;
						return true;
					}
				}
			}
			return false;
		}

		public IEqualityComparer<TKey> Comparer {
			get { return this.comparer; }
		}

		public int Count {
			get { return this.count - this.freeCount; }
		}

		public KeyCollection Keys {
			get { return new KeyCollection(this); }
		}

		public ValueCollection Values {
			get { return new ValueCollection(this); }
		}

		public bool IsReadOnly {
			get { return false; }
		}

		public void Clear() {
			if (this.count > 0) {
				Array.Clear(this.buckets, 0, this.buckets.Length);
				Array.Clear(this.entries, 0, this.count);
				this.count = 0;
			}
			this.freeList = -1;
			this.freeCount = 0;
			this.version++;
		}

		public Enumerator GetEnumerator() {
			return new Enumerator(this);
		}

		// The checks shared by the CopyTo methods
		internal void CheckCopyTo(bool arrayIsNull, int arrayLength, int index) {
			if (arrayIsNull) {
				throw new ArgumentNullException("array");
			}
			if (index < 0 || index > arrayLength) {
				throw new ArgumentOutOfRangeException("index");
			}
			if (arrayLength - index < this.Count) {
				throw new ArgumentException("Destination array is not long enough to copy all the items in the collection.");
			}
		}

		ICollection<TKey> IDictionary<TKey, TValue>.Keys {
			get { return new KeyCollection(this); }
		}

		ICollection<TValue> IDictionary<TKey, TValue>.Values {
			get { return new ValueCollection(this); }
		}

		void ICollection<KeyValuePair<TKey, TValue>>.Add(KeyValuePair<TKey, TValue> item) {
			this.Add(item.Key, item.Value);
		}

		bool ICollection<KeyValuePair<TKey, TValue>>.Contains(KeyValuePair<TKey, TValue> item) {
			int i = this.FindEntry(item.Key);
			return i >= 0 && EqualityComparer<TValue>.Default.Equals(this.entries[i].value, item.Value);
		}

		void ICollection<KeyValuePair<TKey, TValue>>.CopyTo(KeyValuePair<TKey, TValue>[] array, int arrayIndex) {
			this.CheckCopyTo(array == null, array == null ? 0 : array.Length, arrayIndex);
			for (int i = 0; i < this.count; i++) {
				if (this.entries[i].hashCode >= 0) {
					array[arrayIndex++] = new KeyValuePair<TKey, TValue>(this.entries[i].key, this.entries[i].value);
				}
			}
		}

		bool ICollection<KeyValuePair<TKey, TValue>>.Remove(KeyValuePair<TKey, TValue> item) {
			int i = this.FindEntry(item.Key);
			if (i >= 0 && EqualityComparer<TValue>.Default.Equals(this.entries[i].value, item.Value)) {
				this.Remove(item.Key);
				return true;
			}
			return false;
		}

		IEnumerator<KeyValuePair<TKey, TValue>> IEnumerable<KeyValuePair<TKey, TValue>>.GetEnumerator() {
			return new Enumerator(this);
		}

		IEnumerator IEnumerable.GetEnumerator() {
			return new Enumerator(this);
		}

		public bool IsFixedSize {
			get { return false; }
		}

		object IDictionary.this[object key] {
			get {
				if (key is TKey) {
					int i = this.FindEntry((TKey)key);
					if (i >= 0) {
						return this.entries[i].value;
					}
				}
				return null;
			}
			set {
				this[(TKey)key] = (TValue)value;
			}
		}

		ICollection IDictionary.Keys {
			get { return Keys; }
		}

		ICollection IDictionary.Values {
			get { return Values; }
		}

		void IDictionary.Add(object key, object value) {
			this.Add((TKey)key, (TValue)value);
		}

		bool IDictionary.Contains(object key) {
			if (key == null) {
				throw new ArgumentNullException("key");
			}
			return (key is TKey) && this.ContainsKey((TKey)key);
		}

		IDictionaryEnumerator IDictionary.GetEnumerator() {
			return new Enumerator(this);
		}

		void IDictionary.Remove(object key) {
			if (key == null) {
				throw new ArgumentNullException("key");
			}
			if (key is TKey) {
				this.Remove((TKey)key);
			}
		}

		public bool IsSynchronized {
			get { return false; }
		}

		public object SyncRoot {
			get { return this; }
		}

		public void CopyTo(Array array, int index) {
			this.CheckCopyTo(array == null, array == null ? 0 : array.Length, index);
			KeyValuePair<TKey, TValue>[] pairs = array as KeyValuePair<TKey, TValue>[];
			DictionaryEntry[] dictEntries = array as DictionaryEntry[];
			object[] objects = array as object[];
			for (int i = 0; i < this.count; i++) {
				if (this.entries[i].hashCode >= 0) {
					if (pairs != null) {
						pairs[index++] = new KeyValuePair<TKey, TValue>(this.entries[i].key, this.entries[i].value);
					} else if (dictEntries != null) {
						dictEntries[index++] = new DictionaryEntry(this.entries[i].key, this.entries[i].value);
					} else if (objects != null) {
						objects[index++] = new KeyValuePair<TKey, TValue>(this.entries[i].key, this.entries[i].value);
					} else {
						throw new ArgumentException("Target array type is not compatible with the type of items in the collection.");
					}
				}
			}
		}
	}
}
