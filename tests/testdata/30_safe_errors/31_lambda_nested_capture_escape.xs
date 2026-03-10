// Outer lambda captures inner lambda that captures a local — both escape.
// expect-error: does not live long enough

func make_fn() (func () int) {
    var x = 42;
    var inner = func () int {
        return x;
    };
    return func () int {
        return inner();
    };
}

func main() {
    printf("{}\n", make_fn()());
}

