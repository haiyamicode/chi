import "std/ops" as ops;

struct MyInt implements ops.Add {
    value: int = 0;

    func add(rhs: MyInt) MyInt {
        return {.value = this.value + rhs.value};
    }
}

struct Point implements ops.Display, ops.Add {
    x: int;
    y: int;

    func new(x: int, y: int) {
        this.x = x;
        this.y = y;
    }

    func add(other: Point) Point {
        return {this.x + other.x, this.y + other.y};
    }

    func display() string {
        return string.format("({}, {})", this.x, this.y);
    }
}

func add<V: ops.Add>(a: V, b: V) V {
    return a + b;
}

func main() {
    let p1: Point = {0, 1};
    let p2: Point = {2, 3};
    let p3 = p1 + p2;
    printf("p1: {}\n", p1);
    printf("p2: {}\n", p2);
    printf("p3 = p1 + p2: {}\n", p3);
    let i1 = MyInt{.value = 5};
    let i2 = MyInt{.value = 7};
    var result = add<MyInt>(i1, i2);
    printf("MyInt: {} + {} = {}\n", i1.value, i2.value, result.value);
    var result2 = add<int>(10, 15);
    printf("int: 10 + 15 = {}\n", result2);
}

