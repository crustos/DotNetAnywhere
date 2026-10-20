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
using System.Collections.Generic;
using System.Runtime.CompilerServices;
using System.Text;

namespace System {

	public class MissingMethodException : SystemException {
		public MissingMethodException() : base("Attempted to access a missing method.") { }
		public MissingMethodException(string message) : base(message) { }
	}

	public static class Activator {
        [MethodImpl(MethodImplOptions.InternalCall)]
        extern static object Allocate(Type type);

        [MethodImpl(MethodImplOptions.InternalCall)]
        extern static System.Reflection.MethodBase DefaultConstructor(Type type);

        /// <summary>A new instance with its parameterless constructor run (field initialisers included); a value type without one comes back zeroed.</summary>
        public static object CreateInstance(Type type) {
            if (type == null) throw new ArgumentNullException("type");
            object o = Allocate(type);
            System.Reflection.MethodBase ctor = DefaultConstructor(type);
            if (ctor != null) ctor.Invoke(o, new object[0]);
            else if (!type.IsValueType) throw new MissingMethodException("No parameterless constructor defined for type '" + type.FullName + "'.");
            return o;
        }

        // `new T()` in a generic method compiles to this
        public static T CreateInstance<T>() {
            return (T)CreateInstance(typeof(T));
        }
	}

}
