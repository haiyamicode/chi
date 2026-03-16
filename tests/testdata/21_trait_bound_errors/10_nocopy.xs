// Copying a NoCopy type via var init must be rejected
// expect-error: cannot be copied
import "std/ops" as ops;

struct Handle {
    value: int = 0;

    mut func delete() {}

    impl ops.NoCopy {}
}

func main() {
    var a = Handle{};
    var b = a; // copy via var init — rejected
}
