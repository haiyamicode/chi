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
}

