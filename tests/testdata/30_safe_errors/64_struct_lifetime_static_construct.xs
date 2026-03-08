// Constructing a struct with 'static lifetime arg from a local reference.
// The local cannot satisfy 'static — must be rejected.
// expect-error: does not live long enough

struct Holder<'a> {
    ref: &'a int;
}

func make_holder() Holder {
    var x = 42;
    return Holder<'static>{ref: &x};
}

func main() {
    var h = make_holder();
    printf("held: {}\n", *h.ref);
}

