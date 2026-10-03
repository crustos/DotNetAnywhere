using System;

// Delegates bound to virtual and interface methods are created with ldvirtftn, which resolves
// the override from the object's runtime type (and throws NullReferenceException for a null
// receiver, at creation). They used to crash the JIT.
delegate int IntFn();
delegate string StrFn(string s);

class A { public virtual int V() { return 1; } public int NonVirtual() { return 10; } public virtual string Tag(string s) { return "A:" + s; } }
class B : A { public override int V() { return 2; } public override string Tag(string s) { return "B:" + s; } }
class C : B { public override int V() { return 3; } }
class D : A { }                                     // inherits A.V
abstract class Shape { public abstract int Sides(); }
class Tri : Shape { public override int Sides() { return 3; } }
class Quad : Shape { public override int Sides() { return 4; } }

interface IGet { int Get(); }
interface IName { string Name(); }
class Impl : IGet, IName {
    public int Get() { return 7; }
    public string Name() { return "impl"; }
}
class Explicit : IGet { int IGet.Get() { return 8; } }
class Base2 : IGet { public virtual int Get() { return 20; } }
class Derived2 : Base2 { public override int Get() { return 21; } }
class Hidden : Base2 { public new int Get() { return 99; } }   // does not override

class Program {
    static int Call(IntFn f) { return f(); }

    public static int Main() {
        // virtual dispatch through a base-typed reference picks the override
        A a = new B();
        IntFn d1 = a.V; if (d1() != 2) return 1;
        a = new C(); IntFn d2 = a.V; if (d2() != 3) return 2;
        a = new A(); IntFn d3 = a.V; if (d3() != 1) return 3;
        a = new D(); IntFn d4 = a.V; if (d4() != 1) return 4;           // inherited, not overridden
        // the delegate captured the receiver's runtime type, not the static type used afterwards
        A shared = new B(); IntFn d5 = shared.V; shared = new C(); if (d5() != 2) return 5;
        // non-virtual: a plain method group
        IntFn d6 = new A().NonVirtual; if (d6() != 10) return 6;
        // with arguments and a different delegate type
        A t = new B(); StrFn s1 = t.Tag; if (s1("x") != "B:x") return 7;
        // abstract
        Shape[] shapes = new Shape[] { new Tri(), new Quad() };
        IntFn sa = shapes[0].Sides, sb = shapes[1].Sides;
        if (sa() != 3 || sb() != 4) return 8;
        // interface methods, including a second interface on the same class and explicit impls
        IGet g = new Impl(); IntFn i1 = g.Get; if (i1() != 7) return 9;
        IName n = new Impl(); Func<string> i2 = n.Name; if (i2() != "impl") return 10;
        IGet e = new Explicit(); IntFn i3 = e.Get; if (i3() != 8) return 11;
        IGet dd = new Derived2(); IntFn i4 = dd.Get; if (i4() != 21) return 12;
        IGet h = new Hidden(); IntFn i5 = h.Get; if (i5() != 20) return 13;   // interface still maps to Base2.Get
        // passed on and invoked later, multicast
        if (Call(new B().V) != 2) return 14;
        IntFn m = new B().V; m += new C().V; m += new A().V;
        if (m() != 1) return 15;                                         // the last one's result
        // null receiver: creating the delegate throws
        A nul = null; bool threw = false;
        try { IntFn bad = nul.V; } catch (NullReferenceException) { threw = true; }
        if (!threw) return 16;
        // Func<> / Action<> over virtual methods
        Func<int> f = new C().V; if (f() != 3) return 17;
        Func<string, string> f2 = new B().Tag; if (f2("q") != "B:q") return 18;
        return 0;
    }
}
