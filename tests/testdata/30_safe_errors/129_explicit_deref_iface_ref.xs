// Explicit deref of an interface reference yields a bare interface, which is abstract.
// expect-error: interface type 'Named' cannot be used directly
interface Named {
    func name() string;
}

struct Cat {
    impl Named { func name() string { return "cat"; } }
}

func greet(n: &Named) {
    printf("{}\n", (*n).name());
}

func main() {
    var c = Cat{};
    greet(&c);
}
