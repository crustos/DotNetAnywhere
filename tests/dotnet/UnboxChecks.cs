using System;

// Casting an object to a value type (unbox.any) must check the object: null is a
// NullReferenceException, a different type an InvalidCastException. The reference runtimes
// accept an exact match, or an enum and its underlying type (either way round, and between two
// enums sharing it); nothing else, not even int <-> uint. Casting to a reference type is a
// castclass. DNA used to do none of this. The output is compared with Mono's, line for line.
enum E : int { A = 5 }
enum H : int { Z = 6 }
enum F : byte { B = 7 }
enum G : uint { C = 9 }
struct S { public int x; }
struct S2 { public int x; }
class Animal { }
class Dog : Animal { }
class Cat : Animal { }
interface IThing { }
class Thing : IThing { }

class P {
    static string T(Func<object> f) {
        try { return "ok:" + f(); }
        catch (InvalidCastException) { return "InvalidCast"; }
        catch (NullReferenceException) { return "NullRef"; }
    }
    static T Cast<T>(object o) { return (T)o; }
    static void Row(string name, Func<object> f) { Console.WriteLine(name + " " + T(f)); }

    static void Main() {
        object be = E.A, bf = F.B, bg = G.C, bi = 5, bu = 5u, bs = (short)5, bl = 5L, bb = (byte)5, bsb = (sbyte)5,
               bus = (ushort)5, bn = null, bstr = "x", bd = 2.5, bfl = 2.5f, bc = 'c', bbool = true, bS = new S();
        Row("enum(int)->int", () => (int)be);
        Row("int->enum(int)", () => (E)bi);
        Row("enum(int)->enum(int)", () => (H)be);
        Row("enum(byte)->byte", () => (byte)bf);
        Row("byte->enum(byte)", () => (F)bb);
        Row("enum(uint)->uint", () => (uint)bg);
        Row("enum(int)->uint", () => (uint)be);
        Row("enum(int)->enum(uint)", () => (G)be);
        Row("enum(int)->enum(byte)", () => (F)be);
        Row("uint->int", () => (int)bu);
        Row("int->uint", () => (uint)bi);
        Row("short->int", () => (int)bs);
        Row("int->long", () => (long)bi);
        Row("int->short", () => (short)bi);
        Row("sbyte->byte", () => (byte)bsb);
        Row("ushort->short", () => (short)bus);
        Row("int->double", () => (double)bi);
        Row("double->float", () => (float)bd);
        Row("float->double", () => (double)bfl);
        Row("char->int", () => (int)bc);
        Row("bool->int", () => (int)bbool);
        Row("string->int", () => (int)bstr);
        Row("null->int", () => (int)bn);
        Row("S->int", () => (int)bS);
        Row("S->S", () => ((S)bS).x);
        Row("S->S2", () => ((S2)bS).x);
        Row("int->string", () => (string)bi);
        Row("string->string", () => (string)bstr);
        Row("null->string", () => (string)bn);
        Row("int->object", () => (object)bi);
        Row("string->object", () => (object)bstr);

        // reference types: castclass semantics (null passes; wrong type is refused)
        object dog = new Dog(), cat = new Cat(), thing = new Thing();
        Row("Dog->Animal", () => (Animal)dog != null);
        Row("Dog->Dog", () => (Dog)dog != null);
        Row("Dog->Cat", () => (Cat)dog);
        Row("Cat->Animal", () => (Animal)cat != null);
        Row("Animal->Dog (a Cat)", () => (Dog)(Animal)cat);
        Row("Thing->IThing", () => (IThing)thing != null);
        Row("Dog->IThing", () => (IThing)dog);
        Row("null->Dog", () => (Dog)bn);
        Row("string->Animal", () => (Animal)bstr);

        // the same through a generic parameter, which is an unbox.any on an open type
        Row("T=int  from int", () => Cast<int>(bi));
        Row("T=int  from string", () => Cast<int>(bstr));
        Row("T=int  from null", () => Cast<int>(bn));
        Row("T=int  from uint", () => Cast<int>(bu));
        Row("T=int  from enum", () => Cast<int>(be));
        Row("T=E    from int", () => Cast<E>(bi));
        Row("T=long from int", () => Cast<long>(bi));
        Row("T=string from string", () => Cast<string>(bstr));
        Row("T=string from int", () => Cast<string>(bi));
        Row("T=string from null", () => Cast<string>(bn));
        Row("T=Animal from Dog", () => Cast<Animal>(dog) != null);
        Row("T=Dog from Cat", () => Cast<Dog>(cat));
        Row("T=S from S", () => Cast<S>(bS).x);
        Row("T=S from S2", () => Cast<S>(new S2()).x);
        Row("T=double from double", () => Cast<double>(bd));
        Row("T=double from float", () => Cast<double>(bfl));
        Row("T=object from anything", () => Cast<object>(bi));
    }
}
