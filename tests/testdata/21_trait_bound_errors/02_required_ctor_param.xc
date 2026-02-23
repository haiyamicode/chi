// Struct with required constructor param does not implement Construct
// expect-error: does not satisfy trait bound
import "std/ops" as ops;

struct RequiredParam {
    x: int;

    func new(x: int) {
        this.x = x;
    }
}

struct Wrapper<T: ops.Construct> {
    item: T = {};
}

func main() {
    var w = Wrapper<RequiredParam>{};
}
