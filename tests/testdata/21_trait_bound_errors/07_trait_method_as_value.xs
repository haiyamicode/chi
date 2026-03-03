// Taking a trait method as a value instead of calling it — must be rejected.
// expect-error: trait method 'hash' on generic type T must be called, not used as a value
import "std/ops" as ops;

func use_hash<T: ops.Hash>(x: T) {
    var f = x.hash;
}

func main() {
    use_hash(42);
}

