// Using a DisallowCopy type as a type argument must be rejected
// expect-error: cannot be copied
import "std/ops" as ops;

struct Handle {
    value: int = 0;

    mut func delete() {}

    impl ops.DisallowCopy {}
}

struct Wrapper<T> {
    data: T;
}

func main() {
    var w = Wrapper<Handle>{data: {}};
}
