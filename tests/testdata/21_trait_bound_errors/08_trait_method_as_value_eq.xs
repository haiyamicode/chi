// Taking eq trait method as a value instead of calling it — must be rejected.
// expect-error: trait method 'eq' on generic type T must be called, not used as a value
import "std/ops" as ops;

func use_eq<T: ops.Eq>(a: T) {
    var f = a.eq;
}

func main() {
    use_eq(42);
}

