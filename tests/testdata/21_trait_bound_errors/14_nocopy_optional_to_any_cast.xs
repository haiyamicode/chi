// Casting a named Optional<NoCopy> value to any must be rejected
// expect-error: cannot be copied
import "std/ops" as ops;

struct MoveOnly {
    value: int;

    impl ops.NoCopy {}
}

func main() {
    var value: ?MoveOnly = MoveOnly{value: 1};
    var boxed = value as any;
    let _ = boxed;
}
