import "std/ops" as ops;

struct PartialDefault {
    x: int;
    y: int;

    mut func new(x: int, y: int = 0) {
        this.x = x;
        this.y = y;
    }
}

struct Wrapper<T: ops.Construct> {
    item: T = {};
}

func main() {
    var w = Wrapper<PartialDefault>{};
}
