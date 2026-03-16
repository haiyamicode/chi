// Passing a NoCopy type by value to a function must be rejected
// expect-error: cannot be copied
import "std/ops" as ops;

struct Handle {
    value: int = 0;

    mut func delete() {}

    impl ops.NoCopy {}
}

func take_handle(h: Handle) {
    println(h.value);
}

func main() {
    var a = Handle{};
    take_handle(a); // copy via param — rejected
}
