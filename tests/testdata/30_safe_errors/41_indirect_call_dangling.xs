// Indirect call through func variable bypasses direct callee resolution.
// The borrow tracker must read lifetime info from the function type.
// expect-error: does not live long enough

func identity(r: &int) &int {
    return r;
}

func dangle() &int {
    var f = identity;
    var local = 42;
    return f(&local);
}

func main() {
    var r = dangle();
    printf("{}\n", *r);
}
