using System;
using System.Collections.Generic;
using System.Reflection;
using System.Runtime.Serialization;

// Field reflection, nested type names and BitConverter's byte methods. Self-checking: returns 0 on success,
// otherwise the number of the failing check. (Real .NET gives the same answers.)
namespace Outer {
    class MarkAttribute : Attribute { }
    [Serializable] class Saved { [Mark] public int marked; public int plain; }
    abstract class Shape { }
    interface IThing { }
    class Base { public int b = 1; private int hidden = 2; public int Hidden() { return hidden; } }
    class Derived : Base {
        public string s = "x";
        public static int count = 7;
        public const int Constant = 9;
        [NonSerialized] public int skip;
        public struct Pair { public int x; public float y; }
        public Pair pair;
    }
    enum Color { Red = 3, Blue = 5 }

    class Program {
        public static int Main() {
            Derived d = new Derived();
            Type t = typeof(Derived);
            FieldInfo[] pub = t.GetFields();
            if (pub.Length != 6) return 1;                                 // s, count, Constant, skip, pair, b (not the private base field)
            if (t.GetField("s").FieldType != typeof(string)) return 2;
            if ((string)t.GetField("s").GetValue(d) != "x") return 3;
            t.GetField("s").SetValue(d, "y");
            if (d.s != "y") return 4;
            t.GetField("b").SetValue(d, 41);
            if (d.b != 41 || (int)t.GetField("b").GetValue(d) != 41) return 5;
            if (!t.GetField("count").IsStatic || (int)t.GetField("count").GetValue(null) != 7) return 6;
            t.GetField("count").SetValue(null, 8);
            if (Derived.count != 8) return 7;
            if (t.GetField("hidden") != null) return 8;
            if (!t.GetField("Constant").IsLiteral || (int)t.GetField("Constant").GetValue(null) != 9) return 23;
            try { t.GetField("Constant").SetValue(null, 1); return 24; } catch (FieldAccessException) { }
            FieldInfo hidden = typeof(Base).GetField("hidden", BindingFlags.Instance | BindingFlags.NonPublic);
            if (hidden == null || hidden.IsPublic) return 9;
            hidden.SetValue(d, 12);
            if (d.Hidden() != 12) return 10;
            if (!t.GetField("skip").IsNotSerialized || t.GetField("s").IsNotSerialized) return 11;

            object boxed = new Derived.Pair();
            typeof(Derived.Pair).GetField("x").SetValue(boxed, 5);
            if (((Derived.Pair)boxed).x != 5) return 12;
            d.pair.y = 2.5f;
            Derived.Pair copy = (Derived.Pair)t.GetField("pair").GetValue(d);
            if (copy.y != 2.5f) return 13;
            try { typeof(Derived.Pair).GetField("x").SetValue(boxed, 1L); return 14; } catch (ArgumentException) { }

            if (typeof(Derived.Pair).FullName != "Outer.Derived+Pair" || typeof(Derived.Pair).Namespace != "Outer") return 15;
            if (t.GetNestedType("Pair") != typeof(Derived.Pair)) return 16;
            if (Type.GetType("Outer.Derived+Pair") != typeof(Derived.Pair)) return 17;
            if ((int)typeof(Color).GetField("value__", BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic).GetValue(Color.Blue) != 5) return 18;

#pragma warning disable SYSLIB0050
            Derived blank = (Derived)FormatterServices.GetUninitializedObject(typeof(Derived));
#pragma warning restore SYSLIB0050
            if (blank.s != null || blank.b != 0) return 19;

            MethodInfo found = null;
            foreach (MethodInfo m in typeof(Derived).GetMethods()) if (m.Name == "Hidden") found = m;
            if (found == null || (int)found.Invoke(d, null) != 12) return 25;

            if (!typeof(Saved).IsSerializable || typeof(Derived).IsSerializable || !typeof(int).IsSerializable) return 26;
            if (!typeof(Saved).GetField("marked").IsDefined(typeof(MarkAttribute), false) || typeof(Saved).GetField("plain").IsDefined(typeof(MarkAttribute), false)) return 27;
            if (!typeof(Shape).IsAbstract || typeof(Saved).IsAbstract || !typeof(IThing).IsInterface || !typeof(Saved).IsClass || typeof(Derived.Pair).IsClass) return 28;
            if (!typeof(Derived).IsSubclassOf(typeof(Base)) || typeof(Base).IsSubclassOf(typeof(Derived)) || typeof(Base).IsSubclassOf(typeof(Base))) return 29;

            if (typeof(int[]).Name != "Int32[]" || typeof(int[]).FullName != "System.Int32[]" || typeof(int[]) == typeof(string[])) return 30;
            if (typeof(System.Collections.Generic.List<int>).Name != "List`1" || typeof(Derived.Pair[]).Namespace != "Outer") return 31;
            if ("ab".StartsWith("abc") || "ab".EndsWith("xab") || !"abc".StartsWith("ab") || !"abc".EndsWith("bc")) return 32;

            byte[] four = BitConverter.GetBytes(0x01020304);
            if (four.Length != 4 || four[0] != 4 || BitConverter.ToInt32(four, 0) != 0x01020304) return 20;
            if (BitConverter.ToChar(BitConverter.GetBytes('Z'), 0) != 'Z') return 21;
            if (BitConverter.ToSingle(BitConverter.GetBytes(-1.5f), 0) != -1.5f || BitConverter.ToInt64(BitConverter.GetBytes(-2L), 0) != -2L) return 22;
            return 0;
        }
    }
}
