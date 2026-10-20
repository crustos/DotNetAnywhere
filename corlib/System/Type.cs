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

#if !LOCALTEST

using System.Runtime.CompilerServices;
using System.Reflection;
using System.Collections.Generic;

namespace System {
#pragma warning disable 0660, 0661
    public abstract class Type : MemberInfo {

        private static IDictionary<string, Type> typesByNameCache = new Dictionary<string, Type>();

		public static readonly Type[] EmptyTypes = new Type[0];

		[MethodImpl(MethodImplOptions.InternalCall)]
		extern public static Type GetTypeFromHandle(RuntimeTypeHandle handle);

		public abstract Type BaseType {
			get;
		}

		public abstract bool IsEnum {
			get;
		}

		public abstract string Namespace {
			get;
		}

		public abstract string FullName {
			get;
		}

		public abstract bool IsGenericType {
			get;
		}

		public abstract Type GetGenericTypeDefinition();

		public abstract Type[] GetGenericArguments();

        public abstract Type GetElementType();

        public virtual bool IsArray => GetElementType() != null;

        extern public bool IsValueType {
			[MethodImpl(MethodImplOptions.InternalCall)]
			get;
		}

		public override string ToString() {
			return this.FullName;
		}

        public static Type GetType(string typeName)
        {
            lock (typesByNameCache)
            {
                Type cachedResult;
                if (typesByNameCache.TryGetValue(typeName, out cachedResult))
                {
                    return cachedResult;
                }
            }

            string assemblyName;
            string namespaceQualifiedTypeName;

            if (typeName.IndexOf(',') > 0)
            {
                // Assembly is specified
                var parts = typeName.Split(',');
                assemblyName = parts[1].Trim();
                namespaceQualifiedTypeName = parts[0];
            }
            else
            {
                // No assembly specified
                assemblyName = null;
                namespaceQualifiedTypeName = typeName;
            }

            string[] nesting = namespaceQualifiedTypeName.Split('+');
            if (nesting.Length > 1) {
                Type outer = GetType(assemblyName == null ? nesting[0] : nesting[0] + "," + assemblyName);
                for (int i = 1; i < nesting.Length && outer != null; i++) outer = outer.GetNestedType(nesting[i]);
                return outer;
            }
            var namespaceSplitPoint = namespaceQualifiedTypeName.LastIndexOf('.');
            var namespaceName = namespaceQualifiedTypeName.Substring(0, namespaceSplitPoint).Trim();
            var className = namespaceQualifiedTypeName.Substring(namespaceSplitPoint + 1).Trim();
            var result = GetType(assemblyName, namespaceName, className);

            if (result != null)
            {
                lock (typesByNameCache)
                {
                    typesByNameCache[typeName] = result;
                }
            }

            return result;
        }

        [MethodImpl(MethodImplOptions.InternalCall)]
        extern public PropertyInfo[] GetProperties();

        public MethodInfo GetMethod(string name)
        {
            return (MethodInfo)GetMethodInternal(name);
        }

        public FieldInfo[] GetFields() {
            return GetFields(BindingFlags.Instance | BindingFlags.Static | BindingFlags.Public);
        }

        // Declaration order, the type's own fields before its base types'; private fields of base types are not
        // visible, as in .NET.
        public FieldInfo[] GetFields(BindingFlags bindingAttr) {
            List<FieldInfo> ret = new List<FieldInfo>();
            for (Type t = this; t != null; t = t.BaseType) {
                IntPtr[] fields = t.Internal_GetFields();
                for (int i = 0; i < fields.Length; i++) {
                    FieldInfo f = new FieldInfo(t, fields[i]);
                    bool kind = (bindingAttr & (f.IsStatic ? BindingFlags.Static : BindingFlags.Instance)) != 0;
                    bool access = (bindingAttr & (f.IsPublic ? BindingFlags.Public : BindingFlags.NonPublic)) != 0;
                    bool visible = t == this || (!f.IsPrivate && (!f.IsStatic || (bindingAttr & BindingFlags.FlattenHierarchy) != 0));
                    if (kind && access && visible) ret.Add(f);
                }
                if ((bindingAttr & BindingFlags.DeclaredOnly) != 0) break;
            }
            return ret.ToArray();
        }

        public FieldInfo GetField(string name) {
            return GetField(name, BindingFlags.Instance | BindingFlags.Static | BindingFlags.Public);
        }

        public FieldInfo GetField(string name, BindingFlags bindingAttr) {
            foreach (FieldInfo f in GetFields(bindingAttr)) {
                if (f.Name == name) return f;
            }
            return null;
        }

        // ponytail: the public methods of the type and its bases, so an override is listed twice (once per class that declares it).
        public MethodInfo[] GetMethods() {
            List<MethodInfo> ret = new List<MethodInfo>();
            for (Type t = this; t != null; t = t.BaseType) ret.AddRange(t.Internal_GetMethods());
            return ret.ToArray();
        }

        [MethodImpl(MethodImplOptions.InternalCall)]
        extern private MethodInfo[] Internal_GetMethods();

        [MethodImpl(MethodImplOptions.InternalCall)]
        extern private int Internal_TypeFlags();
        [MethodImpl(MethodImplOptions.InternalCall)]
        extern internal static bool Internal_FieldHasAttribute(IntPtr field, Type attributeType);

        /// <summary>[Serializable] is not a custom attribute in metadata but a flag of the type.</summary>
        public bool IsSerializable { get { return (Internal_TypeFlags() & 0x2000) != 0 || IsEnum || (IsValueType && Namespace == "System"); } }
        public bool IsAbstract { get { return (Internal_TypeFlags() & 0x80) != 0; } }
        public bool IsInterface { get { return (Internal_TypeFlags() & 0x20) != 0; } }
        public bool IsClass { get { return !IsInterface && !IsValueType; } }
        public bool IsSealed { get { return (Internal_TypeFlags() & 0x100) != 0; } }

        /// <summary>Is `c` this type or derived from it (interfaces left out: only the base chain is walked).</summary>
        public bool IsSubclassOf(Type c) {
            for (Type t = BaseType; t != null; t = t.BaseType) if (t == c) return true;
            return false;
        }

        public Type GetNestedType(string name) {
            return Internal_GetNestedType(name);
        }

        [MethodImpl(MethodImplOptions.InternalCall)]
        extern internal IntPtr[] Internal_GetFields();
        [MethodImpl(MethodImplOptions.InternalCall)]
        extern internal Type Internal_GetNestedType(string name);
        [MethodImpl(MethodImplOptions.InternalCall)]
        extern internal static string Internal_FieldName(IntPtr field);
        [MethodImpl(MethodImplOptions.InternalCall)]
        extern internal static Type Internal_FieldType(IntPtr field);
        [MethodImpl(MethodImplOptions.InternalCall)]
        extern internal static int Internal_FieldFlags(IntPtr field);
        [MethodImpl(MethodImplOptions.InternalCall)]
        extern internal static object Internal_GetFieldValue(IntPtr field, object obj);
        [MethodImpl(MethodImplOptions.InternalCall)]
        extern internal static void Internal_SetFieldValue(IntPtr field, object obj, object value);
        [MethodImpl(MethodImplOptions.InternalCall)]
        extern internal static object Internal_NewObject(Type type);
        [MethodImpl(MethodImplOptions.InternalCall)]
        extern private object Internal_TakeTypeInitializer();

        // runs the static constructor once, before reflection touches a static field (as ldsfld does)
        internal void EnsureInitialized() {
            MethodInfo cctor = (MethodInfo)Internal_TakeTypeInitializer();
            if (cctor != null) cctor.Invoke(null, null);
        }

        [MethodImpl(MethodImplOptions.InternalCall)]
        extern public static void EnsureAssemblyLoaded(string assemblyName);

        [MethodImpl(MethodImplOptions.InternalCall)]
        extern private static Type GetType(string assemblyName, string namespaceName, string className);

        [MethodImpl(MethodImplOptions.InternalCall)]
        extern private object GetMethodInternal(string name);

        // Null-safe. The previous version, `t1?.FullName.Equals(t2?.FullName) == true`, made
        // (Type)null == null evaluate to false, and `null != t` false as well.
        public static bool operator ==(Type t1, Type t2)
        {
            if (object.ReferenceEquals(t1, t2))
            {
                return true; // the same object, or both null
            }
            if (object.ReferenceEquals(t1, null) || object.ReferenceEquals(t2, null))
            {
                return false;
            }
            return t1.FullName.Equals(t2.FullName);
        }

        public static bool operator !=(Type t1, Type t2)
        {
            return !(t1 == t2);
        }
    }
#pragma warning restore 0660, 0661
}

#endif
