#if !LOCALTEST

using System.Collections;
using System.Collections.Generic;
using System.IO;
using System.Reflection;

namespace System.Runtime.Serialization.Formatters.Binary {
	// ponytail: DNA's own format, not .NET's binary format (MS-NRBF): a program reads back what it wrote, not files
	// written by .NET. The graph is written as a tree (a shared object is written once per reference; a cycle never
	// ends), and the runtime type is recorded only where the declared type is a non-generic class, so a field declared as
	// an array, List<T> or Dictionary<K, V> holds exactly that type (an array or generic instance in an object field
	// throws). Upgrade path: object ids and type descriptors, as MS-NRBF has.
	public sealed class BinaryFormatter {
		static readonly byte[] Magic = { (byte)'D', (byte)'N', (byte)'A', (byte)'B', (byte)'F', 1 };
		const BindingFlags AllInstance = BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic;

		public void Serialize(Stream serializationStream, object graph) {
			if (serializationStream == null) throw new ArgumentNullException("serializationStream");
			List<byte> o = new List<byte>(Magic);
			Write(o, typeof(object), graph);
			byte[] bytes = o.ToArray();
			serializationStream.Write(bytes, 0, bytes.Length);
		}

		public object Deserialize(Stream serializationStream) {
			if (serializationStream == null) throw new ArgumentNullException("serializationStream");
			List<byte> all = new List<byte>();
			byte[] buf = new byte[4096];
			int n;
			while ((n = serializationStream.Read(buf, 0, buf.Length)) > 0) {
				for (int i = 0; i < n; i++) all.Add(buf[i]);
			}
			Reader r = new Reader(all.ToArray());
			for (int i = 0; i < Magic.Length; i++) {
				if (r.Byte() != Magic[i]) throw new SerializationException("The stream was not written by this BinaryFormatter.");
			}
			return r.Value(typeof(object));
		}

		static bool Exact(Type declared) {
			return declared.IsValueType || declared == typeof(string) || declared.IsArray || declared.IsGenericType;
		}

		static void Int(List<byte> o, long v, int size) {
			for (int i = 0; i < size; i++) o.Add((byte)(v >> (8 * i)));
		}

		static void Str(List<byte> o, string s) {
			if (s == null) { Int(o, -1, 4); return; }
			Int(o, s.Length, 4);
			for (int i = 0; i < s.Length; i++) Int(o, s[i], 2);
		}

		static void Write(List<byte> o, Type declared, object v) {
			if (Exact(declared)) { WriteExact(o, declared, v); return; }
			if (v == null) { o.Add(0); return; }
			if (v.GetType().IsArray || v.GetType().IsGenericType) {
				throw new SerializationException("A field declared as " + declared.FullName + " holds a " + v.GetType().FullName + ": this BinaryFormatter records only non-generic types by name.");
			}
			o.Add(1);
			Str(o, v.GetType().FullName);
			WriteExact(o, v.GetType(), v);
		}

		static void WriteExact(List<byte> o, Type t, object v) {
			if (t == typeof(bool)) o.Add((byte)((bool)v ? 1 : 0));
			else if (t == typeof(byte)) o.Add((byte)v);
			else if (t == typeof(sbyte)) o.Add((byte)(sbyte)v);
			else if (t == typeof(char)) Int(o, (char)v, 2);
			else if (t == typeof(short)) Int(o, (short)v, 2);
			else if (t == typeof(ushort)) Int(o, (ushort)v, 2);
			else if (t == typeof(int)) Int(o, (int)v, 4);
			else if (t == typeof(uint)) Int(o, (uint)v, 4);
			else if (t == typeof(long)) Int(o, (long)v, 8);
			else if (t == typeof(ulong)) Int(o, (long)(ulong)v, 8);
			else if (t == typeof(float)) Int(o, BitConverter.SingleToInt32Bits((float)v), 4);
			else if (t == typeof(double)) Int(o, BitConverter.DoubleToInt64Bits((double)v), 8);
			else if (t == typeof(string)) Str(o, (string)v);
			else if (!t.IsValueType && v == null) Int(o, -1, 4);
			else if (t.IsArray) {
				Array a = (Array)v;
				Int(o, a.Length, 4);
				for (int i = 0; i < a.Length; i++) Write(o, t.GetElementType(), a.GetValue(i));
			} else if (t.IsGenericType && t.GetGenericTypeDefinition() == typeof(Dictionary<,>)) {
				IDictionary d = (IDictionary)v;
				Type[] kv = t.GetGenericArguments();
				Int(o, d.Count, 4);
				IDictionaryEnumerator e = d.GetEnumerator();
				while (e.MoveNext()) { Write(o, kv[0], e.Key); Write(o, kv[1], e.Value); }
			} else if (t.IsGenericType && t.GetGenericTypeDefinition() == typeof(List<>)) {
				IList l = (IList)v;
				Int(o, l.Count, 4);
				for (int i = 0; i < l.Count; i++) Write(o, t.GetGenericArguments()[0], l[i]);
			} else {
				if (!t.IsValueType) Int(o, 0, 4);
				foreach (FieldInfo f in t.GetFields(AllInstance)) {
					if (!f.IsNotSerialized) Write(o, f.FieldType, f.GetValue(v));
				}
			}
		}

		sealed class Reader {
			readonly byte[] b;
			int at;

			public Reader(byte[] b) { this.b = b; }

			public byte Byte() {
				if (at >= b.Length) throw new SerializationException("End of Stream encountered before parsing was completed.");
				return b[at++];
			}

			long Int(int size) {
				long v = 0;
				for (int i = 0; i < size; i++) v |= (long)Byte() << (8 * i);
				return size == 8 ? v : size == 4 ? (int)v : size == 2 ? (short)v : v;
			}

			string Str() {
				int n = (int)Int(4);
				if (n < 0) return null;
				char[] c = new char[n];
				for (int i = 0; i < n; i++) c[i] = (char)Int(2);
				return new string(c, 0, n);
			}

			public object Value(Type declared) {
				if (Exact(declared)) return ValueExact(declared);
				if (Byte() == 0) return null;
				string name = Str();
				Type t = Type.GetType(name);
				if (t == null) throw new SerializationException("Unable to find type '" + name + "'.");
				return ValueExact(t);
			}

			object ValueExact(Type t) {
				if (t == typeof(bool)) return Byte() != 0;
				if (t == typeof(byte)) return Byte();
				if (t == typeof(sbyte)) return (sbyte)Byte();
				if (t == typeof(char)) return (char)Int(2);
				if (t == typeof(short)) return (short)Int(2);
				if (t == typeof(ushort)) return (ushort)Int(2);
				if (t == typeof(int)) return (int)Int(4);
				if (t == typeof(uint)) return (uint)Int(4);
				if (t == typeof(long)) return Int(8);
				if (t == typeof(ulong)) return (ulong)Int(8);
				if (t == typeof(float)) return BitConverter.Int32BitsToSingle((int)Int(4));
				if (t == typeof(double)) return BitConverter.Int64BitsToDouble(Int(8));
				if (t == typeof(string)) return Str();
				if (!t.IsValueType) {
					int n = (int)Int(4);
					if (n < 0) return null;
					if (t.IsArray) {
						Array a = Array.CreateInstance(t.GetElementType(), n);
						for (int i = 0; i < n; i++) a.SetValue(Value(t.GetElementType()), i);
						return a;
					}
					if (t.IsGenericType && t.GetGenericTypeDefinition() == typeof(Dictionary<,>)) {
						IDictionary d = (IDictionary)Activator.CreateInstance(t);
						Type[] kv = t.GetGenericArguments();
						for (int i = 0; i < n; i++) { object k = Value(kv[0]); d.Add(k, Value(kv[1])); }
						return d;
					}
					if (t.IsGenericType && t.GetGenericTypeDefinition() == typeof(List<>)) {
						IList l = (IList)Activator.CreateInstance(t);
						for (int i = 0; i < n; i++) l.Add(Value(t.GetGenericArguments()[0]));
						return l;
					}
				}
				object v = FormatterServices.GetUninitializedObject(t);
				foreach (FieldInfo f in t.GetFields(AllInstance)) {
					if (!f.IsNotSerialized) f.SetValue(v, Value(f.FieldType));
				}
				return v;
			}
		}
	}
}

#endif
