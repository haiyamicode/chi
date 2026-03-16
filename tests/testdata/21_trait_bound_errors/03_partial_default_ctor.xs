// Struct with some required and some default params — not fully constructible
// expect-error: does not satisfy trait bound
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
