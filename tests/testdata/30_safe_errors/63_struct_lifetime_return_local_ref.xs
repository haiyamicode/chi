// Struct with lifetime param: returning a struct that borrows a local param.
// The reference escapes the function — must be rejected.
// expect-error: does not live long enough

struct Holder<'a> {
    ref: &'a int;
}

func make_holder(x: int) Holder {
    return {ref: &x};
}

func main() {
    var h = make_holder(5);
    printf("held: {}\n", *h.ref);
}
