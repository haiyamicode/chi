// Copying a DisallowCopy type via assignment must be rejected
// expect-error: cannot be copied
import "std/ops" as ops;

struct Handle {
    value: int = 0;

    mut func delete() {}

    impl ops.DisallowCopy {}
}

func main() {
    var a = Handle{};
    var b = Handle{};
    b = a; // copy via assignment — rejected
}

