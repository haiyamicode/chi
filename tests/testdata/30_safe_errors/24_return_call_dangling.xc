// Return a function call that produces a dangling reference.
// The return expression is a FnCallExpr, not a simple Identifier.
// expect-error: does not live long enough

func identity(r: &int) &int {
    return r;
}

func get_dangling() &int {
    var x = 42;
    return identity(&x);
}

func main() {
    printf("{}\n", get_dangling()!);
}

