// Type satisfies Show but not Construct — must be rejected
// expect-error: does not satisfy trait bound
import "std/ops" as ops;

interface Show {
    func show() string;
}

struct Showable {
    x: int;

    func new(x: int) {
        this.x = x;
    }

    impl Show {
        func show() string {
            return stringf("{}", this.x);
        }
    }
}

struct Holder<T: Show + ops.Construct> {
    item: T = {};
}

func main() {
    var h = Holder<Showable>{};
}

