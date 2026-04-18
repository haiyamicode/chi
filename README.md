# Chi Programming Language

Chi is a statically typed, compiled systems programming language with a dual memory model. Write low-level systems code in **System Chi** (`.xs`) with manual memory control, or high level code in **Application Chi** (`.x`) with automatic memory management — in the same language, with the same syntax.

Chi compiles to native executables via LLVM.

---

## Features

- ⚡ **System mode** (`.xs`) — manual memory, zero-cost abstractions, and direct hardware control; **C++ like performance.**
- 🪄 **Application mode** (`.x`) — garbage collected, no need to worry about memory; **Go-like ergonomics.**
- ⏳ **First-class async/await** — write concurrent code the right way; **Typescript-like async-await.**
- 🛡️ **Memory safety** — compiler-enforced borrow checking for low-level code; **Rust-like safety.**
- **Enum structs** — variants carry data fields and share a common method body
- **Advanced Generics** with trait bounds and operator overloading
- **Fluent error handling** — early-return or wrap the error into `Result<T,E>`, both are supported fluently with full stack trace
- **Best-in-class type inference** — generic instantiation, optional type narrowing, and branch-aware type analysis. Explicit type annotations only when necessary.

---

## 🚀 Quick Example

```swift
func main() {
    let n: int = 10;
    let value = fib(n);
    printf("fib: {}\n", value);
}

func fib(n: int) int {
    if n < 2 {
        return n;
    }
    return fib(n - 2) + fib(n - 1);
}
```

---

## 📖 Language Tour

### Structs

```swift
struct Rect {
    w: int = 0;
    h: int = 0;

    impl Shape {
        func area() int {
            return this.w * this.h;
        }

        func name() string {
            return "rect";
        }
    }
}
```

### Interfaces

```swift
interface Shape {
    func area() int;
    func name() string;
}

func print_shape(s: &Shape) {
    printf("{}: area={}\n", s.name(), s.area());
}
```

---

## 🧱 Memory Model

### System Chi (`.xs`) — Manual Memory

You control the memory. RAII via constructors and destructors.

```swift
struct BoxInt {
    data: *int = null;

    mut func new(value: int) {
        this.data = new int{};
        unsafe { *this.data = value; }
    }

    mut func delete() {
        unsafe { delete this.data; }
    }

    func get() int {
        unsafe { return *this.data; }
    }

    impl ops.Copy {
        mut func copy(source: &This) {
            this.data = new int{};
            unsafe { *this.data = *source.data; }
        }
    }
}

var a = BoxInt{42};
var b = a;          // deep copy via ops.Copy
printf("{}, {}\n", a.get(), b.get()); // 42, 42
// destructors free both allocations when they go out of scope
```

#### Smart Pointers

`Box<T>` for unique ownership, `Shared<T>` for reference-counted sharing. Both implement `Deref` so you call methods and access fields directly:

```swift
// Owned polymorphic collection
var shapes: Array<Box<Shape>> = [];
shapes.push(Box<Shape>{new Circle{radius: 5}});
shapes.push(Box<Shape>{new Rect{w: 3, h: 4}});

for s in shapes {
    printf("{}: {}\n", s.name(), s.area());
}

// Shared ownership
var a = Shared<Point>.from_value({x: 1, y: 2});
var b = a;       // ref count incremented
b.x = 10;       // mutates the shared object
printf("{}\n", a.x); // prints 10
```

#### Borrow Checking

The compiler catches dangling references at compile time:

```swift
func bad() &int {
    var local = 42;
    return &local; // error: does not live long enough
}

func make_fn() func () int {
    var secret = 12345;
    return func () int {
        return secret; // error: does not live long enough
    };
}
```

### Application Chi (`.x`) — Managed Memory

In Managed Mode, the compiler performs **escape analysis** — when a reference to a local variable outlives its scope, the compiler automatically promotes it to be managed by the GC:

```swift
struct Point { x: int; y: int; }

func get_point() &Point {
    var p = Point{x: 1, y: 2};
    return &p; // p escapes → promoted to GC heap automatically
}
```

#### Async/Await

```swift
async func fetch_user(id: int) Promise<string> {
    var data = await db.query(id);
    return data.name;
}

async func process() Promise {
    var a = await fetch_user(1);
    var b = await fetch_user(2);
    printf("{}, {}\n", a, b);
}
```

---

## 📦 Standard Library

| Module | Purpose |
|--------|---------|
| `std/ops` | Core interfaces: `Copy`, `NoCopy`, `Iterator`, `MutIterator`, `Add`, `Show`, `Deref`, `DerefMut`, … |
| `std/io` | `Read`, `Write`, `Close` interfaces; `Buffer` |
| `std/fs` | File system |
| `std/path` | Path manipulation |
| `std/json` | JSON parsing and validation |
| `std/math` | Math functions |
| `std/conv` | Type conversions |
| `std/atomic` | Atomic operations |
| `std/time` | Time utilities |
| `std/args` | Command-line argument parsing |
| `std/os` | OS utilities |
| `std/reflect` | Reflection |

Built-in generic types: `Array<T>`, `Map<K, V>`, `Box<T>`, `Shared<T>`, `Result<T, E>`, `Tuple<...>`.

---

## 🔨 Building & Development

See [Building](docs/building.md) and [Development](docs/development.md).