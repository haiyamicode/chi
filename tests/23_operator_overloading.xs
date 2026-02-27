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

    func new(x: int, y: int) {
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

    func new(p: Point) {
        this._data = p;
    }

    impl ops.UnwrapMut<Point> {
        mut func unwrap_mut() &mut Point {
            return &mut this._data;
        }
    }
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
}

