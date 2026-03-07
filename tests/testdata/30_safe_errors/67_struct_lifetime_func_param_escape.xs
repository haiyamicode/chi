// Struct with lifetime param: function returns struct borrowing a local variable.
// The struct escapes the function while the local is destroyed.
// expect-error: does not live long enough

struct Holder<'a> {
    ref: &'a int;
}

func make() Holder {
    var local = 99;
    var h = Holder{ref: &local};
    return h;
}

func main() {
    var h = make();
    printf("{}\n", *h.ref);
}
