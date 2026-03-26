// Struct containing references as generic type argument is allowed, but escaping a local borrow
// through it must still be rejected.
// expect-error: does not live long enough

struct Wrapper {
    ref: &int;

    mut func new(r: &'this int) {
        this.ref = r;
    }
}

func identity<T>(val: T) T {
    return val;
}

func leak() Wrapper {
    var x = 42;
    var w = Wrapper{&x};
    return identity<Wrapper>(w);
}

func main() {
    let _ = leak();
}
