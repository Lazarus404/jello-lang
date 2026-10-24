# Objects and prototypes

An Object is a mutable map from atoms to values, shared by reference. A prototype is a named Object
that other Objects delegate to: it holds the default fields and the methods that the Objects made
from it share, and gives those Objects a type of their own that the compiler checks method calls
against.

## Objects

An object literal, `{ name: value, ... }`, makes a new Object. Its fields hold boxed values, so a
field reads as a `Dynamic`; `o.get<T>(key)` reads it as a `T`, which the VM checks. Fields can be
added, changed and removed at any time.

```jello
// expect: 3 two true
let o = { a: 1, b: "two" };
o.c = true;
let sum = o.get<I32>(:a) + 2;
Std.string(sum) + " " + (o.b as Bytes) + " " + Std.string(o.exists(:c))
```

Object literals, keys, spreads, the `Object` functions and Object patterns are described in
[Collections](../02-types/03-collections.md). Objects compare by identity (see
[Equality](01-operators.md#equality)).

A call `o.name(args)` whose name is not one of the `Object` functions calls the function held in
the field `name`, passing `o` as its first argument, followed by the arguments. Its result is
checked by the VM against the type the call is used as, so it needs a context that gives one.

```jello
// expect: 2 5
let counter = { count: 0 };
counter.step = fn(self: Object, by: I32): I32 {
  let next = self.get<I32>(:count) + by;
  self.count = next;
  next
};
let first: I32 = counter.step(2);
let second: I32 = counter.step(3);
Std.string(first) + " " + Std.string(second)
```

## Prototype declarations

`prototype Name { ... };` declares a prototype at the top level of a module. Its body is a list of
entries, each ending with `;`:

- `name: value;` is a field, which every Object made from the prototype reads unless it has a field
  of that name of its own.
- `name(params): Result { body };` is a method. Its parameters must have types; the result type can
  be left out, and is then inferred from the body.
- `__proto__: Other;` names the prototype, or any Object, that this one delegates to.

```jello
// expect: Ada is 37
prototype Person {
  name: "nobody";
  age: 0;
  init(name: Bytes, age: I32) { this.name = name; this.age = age; };
  describe(): Bytes { (this.name as Bytes) + " is " + Std.string(this.age as I32) };
};
let ada = new Person("Ada", 37);
ada.describe()
```

A field's value is any expression, evaluated once when the declaration runs. The name of the
prototype is a constant: it holds the prototype Object itself, and is also the type of the Objects
made from it.

A prototype declaring a field twice, or delegating to itself through a chain of `__proto__`
entries, is an error.

## new

`new P(args)` makes an Object that delegates to the prototype `P`, then calls `P`'s `init` method
on it with the arguments, and gives the Object. The Object's type is `P`.

- Without an `init` method, `new P()` takes no arguments.
- `init` is a method like any other, so its arguments are checked against its parameters, and it
  can have defaults and optional parameters.
- `new` on a plain Object, such as `new {}(1)`, is an error: only a prototype's `init` takes
  arguments.

```jello
// error: 3:9: P has no init method, so `new` passes it no arguments [E0805]
prototype P { a: 1; };
let o = new P(1);
o
```

Postfix operations after the arguments apply to the new Object, so `new P(1).m()` is
`(new P(1)).m()`.

## this

Inside a method, `this` is the Object the method was called on, with the prototype's type. Reading
`this.field` reads the Object's own field or, failing that, the prototype's; assigning
`this.field = value` always sets the Object's own field, leaving the prototype's default alone.

```jello
// expect: 1 0
prototype Counter {
  n: 0;
  inc(): Counter { this.n = (this.n as I32) + 1; this };
};
let c = new Counter();
c.inc();
let fresh = new Counter();
Std.string(c.n as I32) + " " + Std.string(fresh.n as I32)
```

A method that returns `this` can be chained: `c.inc().inc()`.

### method fn

A `fn` expression inside a method does not see the method's `this`. Write `method fn` instead to
make a function that keeps it.

```jello
// expect: 4
prototype Counter {
  n: 3;
  get(): I32 { this.n as I32 };
  next(): I32 {
    let step = method fn(): I32 { this.get() + 1 };
    step()
  };
};
let c = new Counter();
Std.string(c.next())
```

```jello
// error: 3:34: `this` is the `this` of the method around this `fn`, which does not see it; write `method fn` [E0306]
prototype P {
  m(): I32 { let g = fn(): I32 { this.n as I32 }; g() };
};
1
```

## Methods

A call `o.m(args)` on a value of a prototype's type finds `m` on the Object or on a prototype it
delegates to, and is checked against the method's declared parameters and result. Methods can call
each other in any order, including recursively.

```jello
// error: 4:9: wrong argument count for P.m [E0600]
prototype P { m(n: I32): I32 { n }; };
let p = new P();
let x = p.m(1, 2);
x
```

An Object may hold a function of its own in a method's place; a call then runs that function. The
function must have the method's type, with the receiver as its first parameter. A prototype with a
[linked typedef](#linked-typedefs) fixes its methods instead, and so does one that delegates to such
a prototype or to a value the compiler cannot name as a prototype.

```jello
// expect: 2 8
prototype Node {
  init(v: I32) { this.v = v; };
  scaled(by: I32): I32 { (this.v as I32) * by };
};
let a = new Node(1);
let b = new Node(1);
b.scaled = fn(self: Node, by: I32): I32 { 2 * by };
Std.string(a.scaled(2)) + " " + Std.string(b.scaled(4))
```

### Methods as values

Read without a call, `o.m` is the method as a function whose first parameter is the receiver, as in
JavaScript's unbound methods. `P.m` reads it from the prototype itself.

```jello
// expect: 10 13
prototype Counter {
  init(n: I32) { this.n = n; };
  get(): I32 { this.n as I32 };
  add(k: I32): I32 { (this.n as I32) + k };
};
let c = new Counter(3);
let d = new Counter(10);
let get = c.get;
let add = Counter.add;
Std.string(get(d)) + " " + Std.string(add(d, 3))
```

## Delegation and super

A prototype whose `__proto__` is another prototype delegates to it: its Objects find the parent's
fields and methods when they have none of their own. A value of the child's type is also a value of
the parent's type, and can be passed wherever the parent is expected.

In a method, `super.m(args)` calls the method `m` that the prototype's `__proto__` provides, with
the same `this`. It is how an overriding method calls the one it overrides.

```jello
// expect: 30 user
prototype HasId {
  id: 0;
  setId(n: I32) { this.id = n; };
};
prototype User {
  __proto__: HasId;
  name: "";
  setId(n: I32) { super.setId(n * 10); };
  setup(n: I32): User { this.setId(n); this.name = "user"; this };
};
let u = new User().setup(3);
let h: HasId = u;
Std.string(h.id as I32) + " " + (u.name as Bytes)
```

`super` is only a callee: `super.m(...)`. Using it alone, or in a prototype without a `__proto__`,
is an error.

```jello
// error: 3:9: P has no __proto__, so `super` has nothing to call [E0805]
prototype P {
  m() { super.x(); };
};
1
```

### as and is

Nothing records which prototype made an Object, so `value as P` and `value is P` (on a `Dynamic`)
test whether the Object delegates to `P`, directly or through other prototypes. A cast that fails
traps with `TYPE_MISMATCH`.

```jello
// expect: true false true
prototype Animal { legs: 4; };
prototype Dog { __proto__: Animal; };
let d = new Dog() as Dynamic;
let a = new Animal() as Dynamic;
Std.string(d is Animal) + " " + Std.string(a is Dog) + " " + Std.string((d as Animal) == (d as Dog))
```

## bind and callWith

A function's `this` is its first parameter. `Function.bind` and `Function.callWith`, also written
`f.bind(...)` and `f.callWith(...)`, pass it explicitly, as JavaScript's `bind` and `call` do.

- `f.callWith(thisArg, args...)` calls `f` with `thisArg` as its first argument, followed by `args`.
- `f.bind(thisArg, args...)` gives a new function of `f`'s remaining parameters, which calls `f`
  with `thisArg` and `args` first. They are evaluated once, when `bind` runs.

Unlike JavaScript, `bind` on a plain function fills its first parameter, and binding an already
bound function fixes its next parameter rather than being ignored.

```jello
// expect: 3 15 123 145 110
prototype Counter {
  init(n: I32) { this.n = n; };
  get(): I32 { this.n as I32 };
  add(k: I32): I32 { (this.n as I32) + k };
};
let c = new Counter(3);
let d = new Counter(10);
let bound = c.get.bind(c);
let plus = fn(a: I32, b: I32, c: I32): I32 { a * 100 + b * 10 + c };
let p = Function.bind(plus, 1);
let x = 1;
let q = plus.bind(x, x);
x = 9;
Bytes.join([Std.string(bound()), Std.string(c.add.callWith(d, 5)), Std.string(p(2, 3)),
  Std.string(p.bind(4)(5)), Std.string(q(0))], " ")
```

`bind` needs the `this` it fixes, and cannot fix more parameters than the function has before a
variadic one:

```jello
// error: 3:1: Function.bind fixes 2 parameters of (I32) -> I32, which takes 1 [E0600]
let f = fn(a: I32): I32 { a };
Function.bind(f, 1, 2);
1
```

## Linked typedefs

A prototype's fields read as `Dynamic`. To give them static types, declare a record typedef with the
same name as the prototype. The two are linked: the typedef gives the fields of the prototype's
values, which are read and written as their declared types, and the prototype makes them. Either
declaration can come first. See [Typedefs and records](../02-types/06-typedefs-and-records.md) for
record typedefs.

```jello
// expect: 3 40
typedef Node = { let value: I32; let next: ?Node; let age: ?I32; };
prototype Node {
  next: null;
  init(value: I32) { this.value = value; };
  length(): I32 { this.next == null ? 1 : 1 + this.next.length() };
};
let a = new Node(1);
let b = new Node(2);
b.next = a;
let total: I32 = a.value + b.value;
a.age = 40;
Std.string(total) + " " + Std.string(a.age as I32)
```

On a linked value:

- A field holds its declared type, so writing a value of another type is an error.
- Its shape is closed: reading, writing or calling a name that is neither a field nor a method is
  an error, unless the prototype delegates through one without a typedef.
- A nullable field reads null until it is set.
- A field that holds a function is called as itself, without a receiver.
- A name cannot be both a method of the prototype and a field of the typedef.
- The two declarations must take the same type parameters, and be exported together or not at all.
- Its methods are fixed, as a class's are, so a call that can only find one method calls it
  directly, which is as fast as calling a function. Replacing one is an error.

```jello
// error: 5:7: field x of Point is I32, but this value is Bytes [E0400]
typedef Point = { let x: I32; let y: I32; };
prototype Point { x: 0; };
let p = new Point();
p.x = "s";
1
```

```jello
// error: 5:1: area cannot be replaced; a linked prototype's methods, and those of any that may delegate to one, are fixed [E0805]
typedef Shape = { let w: I32; };
prototype Shape { init(w: I32) { this.w = w; }; area(): I32 { this.w * this.w }; };
let s = new Shape(3);
s.area = fn(self: Shape): I32 { 0 };
s.area()
```

A prototype that delegates to a linked one, or to a value the compiler cannot name as a prototype,
fixes its methods too, and can still override those it delegates to; a call that such an override
may answer looks the method up as before.

A linked value is still an Object, so it can be passed where an `Object` is expected; read through
an `Object`, its fields are `Dynamic` again. A write through an `Object` or `Dynamic` that would
change which method a linked value finds traps with `SEALED`: replacing or removing a fixed method,
giving an Object that delegates to one a field of the same name, or changing such an Object's
`__proto__`.

## Generic prototypes

A prototype can take type parameters, `prototype Box<T> { ... }`, usually with a linked typedef of
the same parameters. Each list of type arguments the program uses, such as `Box<I32>`, is a
prototype of its own, with its own methods checked for those types.

`new Box<I32>(7)` names the type arguments; `new Box(7)` infers them from the arguments to `init`,
as a generic call does, or from the type the value is expected to have.

```jello
// expect: 10 yo true false
typedef Box<T> = { let value: T; };
prototype Box<T> {
  init(value: T) { this.value = value; };
  get(): T { this.value };
  pair(other: T): Tuple<T, T> { (this.get(), other) };
};
let a = new Box<I32>(7);
let b = new Box("hi");
let wide: Box<I64> = new Box(3);
let sum = a.get() + (wide.get() as I32);
Bytes.join([Std.string(sum), b.pair("yo").1, Std.string((a as Dynamic) is Box<I32>),
  Std.string((b as Dynamic) is Box<I32>)], " ")
```

A generic prototype as a value needs its type arguments, so `__proto__: Box` and `let p = Box;` are
errors; `new Box()` with nothing to infer `T` from is an error asking for an annotation.

```jello
// error: 5:9: cannot infer T of Box here; annotate it [E0403]
prototype Box<T> {
  a: 0;
};
let o = new Box();
o
```

## Importing prototypes

An exported prototype is both a type and a value, so a named import brings in both, and a
namespace import reaches it as `M.Name`, in types and in `new M.Name(args)`. A linked typedef and
its prototype are exported and imported as one name, and so is a generic prototype with all its
instances.

```jello file=lib/Shapes.jello
package lib;

export typedef Shape = { let sides: I32; };
export prototype Shape {
  sides: 3;
  init(sides: I32) { this.sides = sides; };
  more(): I32 { this.sides + 1 };
};
```

```jello
// flags: --path $DIR
// expect: 5 7
import { Shape } from lib.Shapes;
import lib.Shapes as S;
let square = new Shape(4);
let hexagon: Shape = new S.Shape(6);
Std.string(square.more()) + " " + Std.string(hexagon.more())
```

See [Modules](../05-modules.md) for imports and exports.
