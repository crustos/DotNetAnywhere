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
using System.Text;
using System.Collections.Generic;

namespace System {

	class RuntimeType : Type {

		[MethodImpl(MethodImplOptions.InternalCall)]
		extern private RuntimeType GetNestingParentType();

		[MethodImpl(MethodImplOptions.InternalCall)]
		extern private RuntimeType Internal_GetGenericTypeDefinition();

		extern public override Type BaseType {
			[MethodImpl(MethodImplOptions.InternalCall)]
			get;
		}

		public extern override bool IsEnum {
			[MethodImpl(MethodImplOptions.InternalCall)]
			get;
		}

		[MethodImpl(MethodImplOptions.InternalCall)]
		extern private string Internal_GetNamespace();

		[MethodImpl(MethodImplOptions.InternalCall)]
		extern private string Internal_GetName();

		// An array is named after its element, as in .NET (`Int32[]`, `System.Int32[]`): every array type used to be `System.Array`, and so equal to
		// every other one (Type == compares full names).  A generic instance's Name leaves out its arguments (`List`1`); FullName has them.
		public override string Namespace {
			get { Type e = GetElementType(); return e != null ? e.Namespace : Internal_GetNamespace(); }
		}

		public override string Name {
			get {
				Type e = GetElementType();
				if (e != null) return e.Name + "[]";
				string n = Internal_GetName();
				int tick = n.IndexOf('`');
				int open = tick < 0 ? -1 : n.IndexOf('[', tick);
				return open < 0 ? n : n.Substring(0, open);
			}
		}

		public override string FullName {
			get {
				Type e = GetElementType();
				if (e != null) return e.FullName + "[]";
				StringBuilder ret = new StringBuilder(32);
				// A type in the global namespace has no "namespace." prefix (this used to give ".Name")
				string ns = this.Namespace;
				if (ns != null && ns.Length > 0) {
					ret.Append(ns);
					ret.Append('.');
				}
				RuntimeType nestingParentType = this.GetNestingParentType();
				if (nestingParentType != null) {
					List<Type> nestingParents = new List<Type>();
					nestingParents.Add(nestingParentType);
					nestingParentType = nestingParentType.GetNestingParentType();
					while (nestingParentType != null) {
						nestingParents.Add(nestingParentType);
						nestingParentType = nestingParentType.GetNestingParentType();
					}
					for (int ofs = nestingParents.Count - 1; ofs >= 0; ofs--) {
						ret.Append(nestingParents[ofs].Name);
						ret.Append('+');
					}
				}
				ret.Append(Internal_GetName());
				return ret.ToString();
			}
		}

		extern public override bool IsGenericType {
			[MethodImpl(MethodImplOptions.InternalCall)]
			get;
		}

		public override Type GetGenericTypeDefinition() {
			if (!this.IsGenericType) {
				throw new InvalidOperationException("This is not a generic type");
			}
			return this.Internal_GetGenericTypeDefinition();
		}

		[MethodImpl(MethodImplOptions.InternalCall)]
		extern public override Type[] GetGenericArguments();

        [MethodImpl(MethodImplOptions.InternalCall)]
        extern public override Type GetElementType();

    }

}

#endif
