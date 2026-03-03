// Struct containing references as generic type argument is rejected in safe mode.
// expect-error: cannot use borrowing type

struct Wrapper {
    ref: &int;

    mut func new(r: &'this int) {
        this.ref = r;
    }
}

func identity<T>(val: T) T {
    return val;
}

func main() {
    var x = 42;
    var w = Wrapper{&x};
    var w2 = identity<Wrapper>(w);
}

