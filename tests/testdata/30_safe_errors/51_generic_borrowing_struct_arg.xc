// Struct containing references as generic type argument is rejected in safe mode.

struct Wrapper {
    ref: &int = null;
}

func identity<T>(val: T) T { return val; }

func main() {
    var x = 42;
    var w = Wrapper{ref: &x};
    var w2 = identity<Wrapper>(w);
}
