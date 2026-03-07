// Implicit 'this bound: ref field source must outlive the struct instance.
// Reassignment with source declared after the struct variable — must be rejected.
// expect-error: does not live long enough

struct Holder<'a> {
    ref: &'a int;
}

func main() {
    var h: Holder;
    var x = 42;
    h = Holder{ref: &x};
    printf("{}\n", *h.ref);
}
