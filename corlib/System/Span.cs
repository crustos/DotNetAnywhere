namespace System {

	// DNA has no byref-like types, so spans here are a window onto an array, not a
	// pointer. A Span over a T[] aliases it, as in the reference runtime.
	// MemoryMarshal.CreateSpan / AsBytes / Cast produce *copies* (see MemoryMarshal).
	public struct Span<T> {
		internal T[] _array;
		internal int _start;
		internal int _length;

		public Span(T[] array) {
			if (array == null) {
				_array = null; _start = 0; _length = 0;
			} else {
				_array = array; _start = 0; _length = array.Length;
			}
		}

		public Span(T[] array, int start, int length) {
			if (array == null) {
				if (start != 0 || length != 0) {
					throw new ArgumentOutOfRangeException("start");
				}
				_array = null; _start = 0; _length = 0;
				return;
			}
			if (start < 0 || length < 0 || start > array.Length - length) {
				throw new ArgumentOutOfRangeException("start");
			}
			_array = array; _start = start; _length = length;
		}

		public int Length { get { return _length; } }
		public bool IsEmpty { get { return _length == 0; } }
		public static Span<T> Empty { get { return new Span<T>(); } }

		public T this[int index] {
			get {
				if ((uint)index >= (uint)_length) {
					throw new IndexOutOfRangeException();
				}
				return _array[_start + index];
			}
			set {
				if ((uint)index >= (uint)_length) {
					throw new IndexOutOfRangeException();
				}
				_array[_start + index] = value;
			}
		}

		public T[] ToArray() {
			T[] result = new T[_length];
			for (int i = 0; i < _length; i++) {
				result[i] = _array[_start + i];
			}
			return result;
		}

		public Span<T> Slice(int start) {
			return Slice(start, _length - start);
		}

		public Span<T> Slice(int start, int length) {
			if (start < 0 || length < 0 || start > _length - length) {
				throw new ArgumentOutOfRangeException("start");
			}
			Span<T> s = new Span<T>();
			s._array = _array; s._start = _start + start; s._length = length;
			return s;
		}

		public void CopyTo(Span<T> destination) {
			if (destination._length < _length) {
				throw new ArgumentException("Destination is too short.");
			}
			if (destination._array == _array && destination._start > _start) {
				for (int i = _length - 1; i >= 0; i--) {
					destination._array[destination._start + i] = _array[_start + i];
				}
			} else {
				for (int i = 0; i < _length; i++) {
					destination._array[destination._start + i] = _array[_start + i];
				}
			}
		}

		public void Fill(T value) {
			for (int i = 0; i < _length; i++) {
				_array[_start + i] = value;
			}
		}

		public void Clear() {
			Fill(default(T));
		}

		public static implicit operator Span<T>(T[] array) {
			return new Span<T>(array);
		}

		public static implicit operator ReadOnlySpan<T>(Span<T> span) {
			ReadOnlySpan<T> r = new ReadOnlySpan<T>();
			r._array = span._array; r._start = span._start; r._length = span._length;
			return r;
		}
	}

	public struct ReadOnlySpan<T> {
		internal T[] _array;
		internal int _start;
		internal int _length;

		public ReadOnlySpan(T[] array) {
			if (array == null) {
				_array = null; _start = 0; _length = 0;
			} else {
				_array = array; _start = 0; _length = array.Length;
			}
		}

		public ReadOnlySpan(T[] array, int start, int length) {
			if (array == null) {
				if (start != 0 || length != 0) {
					throw new ArgumentOutOfRangeException("start");
				}
				_array = null; _start = 0; _length = 0;
				return;
			}
			if (start < 0 || length < 0 || start > array.Length - length) {
				throw new ArgumentOutOfRangeException("start");
			}
			_array = array; _start = start; _length = length;
		}

		public int Length { get { return _length; } }
		public bool IsEmpty { get { return _length == 0; } }
		public static ReadOnlySpan<T> Empty { get { return new ReadOnlySpan<T>(); } }

		public T this[int index] {
			get {
				if ((uint)index >= (uint)_length) {
					throw new IndexOutOfRangeException();
				}
				return _array[_start + index];
			}
		}

		public T[] ToArray() {
			T[] result = new T[_length];
			for (int i = 0; i < _length; i++) {
				result[i] = _array[_start + i];
			}
			return result;
		}

		public ReadOnlySpan<T> Slice(int start) {
			return Slice(start, _length - start);
		}

		public ReadOnlySpan<T> Slice(int start, int length) {
			if (start < 0 || length < 0 || start > _length - length) {
				throw new ArgumentOutOfRangeException("start");
			}
			ReadOnlySpan<T> s = new ReadOnlySpan<T>();
			s._array = _array; s._start = _start + start; s._length = length;
			return s;
		}

		public void CopyTo(Span<T> destination) {
			if (destination._length < _length) {
				throw new ArgumentException("Destination is too short.");
			}
			for (int i = 0; i < _length; i++) {
				destination._array[destination._start + i] = _array[_start + i];
			}
		}

		public static implicit operator ReadOnlySpan<T>(T[] array) {
			return new ReadOnlySpan<T>(array);
		}
	}
}
