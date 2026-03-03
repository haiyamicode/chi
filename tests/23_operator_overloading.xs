import "std/ops" as ops;

struct MyInt {
    value: int = 0;

    impl ops.Add {
        func add(rhs: MyInt) MyInt {
            return {value: this.value + rhs.value};
        }
    }
}

struct Point {
    x: int;
    y: int;

    mut func new(x: int, y: int) {
        this.x = x;
        this.y = y;
    }

    impl ops.Display {
        func display() string {
            return stringf("({}, {})", this.x, this.y);
        }
    }

    impl ops.Add {
        func add(other: Point) Point {
            return {this.x + other.x, this.y + other.y};
        }
    }
}

func make_point(x: int, y: int) Point {
    return {x, y};
}

func add<V: ops.Add>(a: V, b: V) V {
    return a + b;
}

// Custom wrapper with only UnwrapMut (compiler should use it for reads too)
struct Wrapper {
    private _data: Point;

    mut func new(p: Point) {
        this._data = p;
    }

    impl ops.UnwrapMut<Point> {
        mut func unwrap_mut() &mut Point {
            return &mut this._data;
        }
    }
}

// Vec2: tests all arithmetic, bitwise, shift, and unary operator interfaces
struct Vec2 {
    x: int = 0;
    y: int = 0;

    impl ops.Display {
        func display() string {
            return stringf("[{}, {}]", this.x, this.y);
        }
    }

    impl ops.Add {
        func add(rhs: Vec2) Vec2 {
            return {x: this.x + rhs.x, y: this.y + rhs.y};
        }
    }
    impl ops.Sub {
        func sub(rhs: Vec2) Vec2 {
            return {x: this.x - rhs.x, y: this.y - rhs.y};
        }
    }
    impl ops.Mul {
        func mul(rhs: Vec2) Vec2 {
            return {x: this.x * rhs.x, y: this.y * rhs.y};
        }
    }
    impl ops.Div {
        func div(rhs: Vec2) Vec2 {
            return {x: this.x / rhs.x, y: this.y / rhs.y};
        }
    }
    impl ops.Rem {
        func rem(rhs: Vec2) Vec2 {
            return {x: this.x % rhs.x, y: this.y % rhs.y};
        }
    }
    impl ops.Neg {
        func neg() Vec2 {
            return {x: -this.x, y: -this.y};
        }
    }
    impl ops.BitAnd {
        func bitand(rhs: Vec2) Vec2 {
            return {x: this.x & rhs.x, y: this.y & rhs.y};
        }
    }
    impl ops.BitOr {
        func bitor(rhs: Vec2) Vec2 {
            return {x: this.x | rhs.x, y: this.y | rhs.y};
        }
    }
    impl ops.BitXor {
        func bitxor(rhs: Vec2) Vec2 {
            return {x: this.x ^ rhs.x, y: this.y ^ rhs.y};
        }
    }
    impl ops.Not {
        func not() Vec2 {
            return {x: ~this.x, y: ~this.y};
        }
    }
    impl ops.Shl {
        func shl(rhs: Vec2) Vec2 {
            return {x: this.x << rhs.x, y: this.y << rhs.y};
        }
    }
    impl ops.Shr {
        func shr(rhs: Vec2) Vec2 {
            return {x: this.x >> rhs.x, y: this.y >> rhs.y};
        }
    }
    impl ops.Eq {
        func eq(other: Vec2) bool {
            return this.x == other.x && this.y == other.y;
        }
    }
    impl ops.Ord {
        // Compare by magnitude squared (x*x + y*y)
        func cmp(other: Vec2) int {
            var lhs = this.x * this.x + this.y * this.y;
            var rhs = other.x * other.x + other.y * other.y;
            return lhs - rhs;
        }
    }
}

func make_vec2(x: int, y: int) Vec2 {
    return {x: x, y: y};
}

func sub<V: ops.Sub>(a: V, b: V) V {
    return a - b;
}

func test_all_operators() {
    println("=== Binary operators ===");
    var a = Vec2{x: 10, y: 20};
    var b = Vec2{x: 3, y: 4};
    printf("add: {}\n", a + b);
    printf("sub: {}\n", a - b);
    printf("mul: {}\n", a * b);
    printf("div: {}\n", a / b);
    printf("rem: {}\n", a % b);

    println("=== Bitwise operators ===");
    var c = Vec2{x: 255, y: 15};
    var d = Vec2{x: 15, y: 255};
    printf("bitand: {}\n", c & d);
    printf("bitor: {}\n", c | d);
    printf("bitxor: {}\n", c ^ d);

    println("=== Shift operators ===");
    var e = Vec2{x: 8, y: 16};
    var f = Vec2{x: 2, y: 3};
    printf("shl: {}\n", e << f);
    printf("shr: {}\n", e >> f);

    println("=== Unary operators ===");
    printf("neg: {}\n", -a);
    var g = Vec2{x: 0, y: -1};
    printf("not: {}\n", ~g);

    println("=== Compound assignments ===");
    var h = Vec2{x: 10, y: 20};
    h += Vec2{x: 5, y: 5};
    printf("+=: {}\n", h);
    h -= Vec2{x: 3, y: 3};
    printf("-=: {}\n", h);
    h *= Vec2{x: 2, y: 2};
    printf("*=: {}\n", h);
    h /= Vec2{x: 3, y: 11};
    printf("/=: {}\n", h);
    h %= Vec2{x: 3, y: 4};
    printf("%=: {}\n", h);
    h &= Vec2{x: 255, y: 255};
    printf("&=: {}\n", h);
    h |= Vec2{x: 16, y: 32};
    printf("|=: {}\n", h);
    h ^= Vec2{x: 255, y: 255};
    printf("^=: {}\n", h);
    var i = Vec2{x: 1, y: 2};
    i <<= Vec2{x: 4, y: 3};
    printf("<<=: {}\n", i);
    i >>= Vec2{x: 2, y: 1};
    printf(">>=: {}\n", i);

    println("=== Generic with Sub ===");
    printf("generic sub: {}\n", sub<Vec2>(a, b));

    println("=== Comparison operators ===");
    var p = Vec2{x: 3, y: 4}; // magnitude^2 = 25
    var q = Vec2{x: 3, y: 4}; // magnitude^2 = 25
    var r = Vec2{x: 1, y: 1}; // magnitude^2 = 2
    var s = Vec2{x: 10, y: 0}; // magnitude^2 = 100
    printf("p == q: {}\n", p == q);
    printf("p == r: {}\n", p == r);
    printf("p != r: {}\n", p != r);
    printf("p != q: {}\n", p != q);
    printf("r < p: {}\n", r < p);
    printf("p < r: {}\n", p < r);
    printf("s > p: {}\n", s > p);
    printf("p > s: {}\n", p > s);
    printf("r <= p: {}\n", r <= p);
    printf("p <= q: {}\n", p <= q);
    printf("s >= p: {}\n", s >= p);
    printf("p >= q: {}\n", p >= q);

    println("=== Temporaries ===");
    printf("call - call: {}\n", make_vec2(10, 20) - make_vec2(3, 4));
    printf("chain: {}\n", make_vec2(1, 2) + make_vec2(3, 4) - make_vec2(1, 1));
}

func main() {
    let p1 = Point{0, 1};
    let p2 = Point{2, 3};
    let p3 = p1 + p2;
    printf("p1: {}\n", p1);
    printf("p2: {}\n", p2);
    printf("p3 = p1 + p2: {}\n", p3);
    let i1 = MyInt{value: 5};
    let i2 = MyInt{value: 7};
    var result = add<MyInt>(i1, i2);
    printf("MyInt: {} + {} = {}\n", i1.value, i2.value, result.value);
    var result2 = add<int>(10, 15);
    printf("int: 10 + 15 = {}\n", result2);

    // Box Unwrap/UnwrapMut
    var b = Box<Point>{new Point{7, 8}};
    printf("box unwrap: {}\n", b!);
    b!.x = 50;
    b!.y = 60;
    printf("box unwrap_mut: {}\n", b!);

    // Shared Unwrap (read-only)
    let sp = Point{3, 4};
    var s = Shared<Point>{sp};
    printf("shared unwrap: {}\n", s!);

    // Wrapper: only UnwrapMut, compiler should use it for reads
    let wp = Point{100, 200};
    var w = Wrapper{wp};
    printf("wrapper read: {}\n", w!);
    w!.x = 300;
    printf("wrapper write: {}\n", w!);

    // Address-of unwrap: &(b!) should give &Point
    let ref = &(b!);
    printf("addr of unwrap: {}\n", *ref);

    // Operator on temporaries (LHS has no address)
    var tmp = Point{10, 20};
    let p4 = tmp + p1;
    printf("tmp + var: {}\n", p4);
    let p5 = make_point(5, 6) + p2;
    printf("call + var: {}\n", p5);

    // Chained temporaries: call + call, call + call + var
    let p6 = make_point(1, 2) + make_point(3, 4);
    printf("call + call: {}\n", p6);
    let p7 = make_point(10, 10) + make_point(20, 20) + p1;
    printf("call + call + var: {}\n", p7);

    // Nested: result of generic add on temporaries
    let p8 = add<Point>(make_point(1, 1), make_point(2, 2));
    printf("generic add calls: {}\n", p8);

    // Deep chain: four temporaries
    let p9 = make_point(1, 0) + make_point(0, 1) + make_point(10, 0) + make_point(0, 10);
    printf("four temps: {}\n", p9);

    // Mixed: temporary op with result feeding into another op with var
    let p10 = make_point(100, 100) + add<Point>(p1, p2);
    printf("temp + generic: {}\n", p10);

    test_all_operators();
}

