import "std/ops" as ops;

struct Point implements ops.Display, ops.Add {
    x: int;
    y: int;

    func new(x: int, y: int) {
        this.x = x;
        this.y = y;
    }

    func add(other: Point) Point {
        return {
            this.x + other.x,
            this.y + other.y,
        };
    }

    func display() string {
        return stringf("({}, {})", this.x, this.y);
    }
}

func main() {
    let p1: Point = {0, 1};
    let p2: Point = {2, 3};
    let p3 = p1 + p2;
    printf("p1: {}\n", p1);
    printf("p2: {}\n", p2);
    printf("p3 = p1 + p2: {}\n", p3);
}