// Nested call chain: wrap(identity(&local)) should be caught.
// The inner call's borrow must propagate through the outer call.
// expect-error: does not live long enough

func identity(r: &int) &int {
    return r;
}

func wrap(r: &int) &int {
    return r;
}

func get_dangling() &int {
    var local = 42;
    return wrap(identity(&local));
}

func main() {
    printf("{}\n", get_dangling()!);
}

