// Implicit 'this bound: inner-scope source assigned to outer-scope struct.
// The source dies when the inner scope ends — must be rejected.
// expect-error: does not live long enough

struct Holder<'a> {
    ref: &'a int;
}

func main() {
    var seed = 0;
    var h: Holder = {ref: &seed};
    {
        var x = 42;
        h = {ref: &x};
    }
    printf("{}\n", *h.ref);
}
