// Indirect call through func variable bypasses direct callee resolution.
// The borrow tracker must read lifetime info from the function type.

func identity(r: &int) &int { return r; }

func dangle() &int {
    var f = identity;
    var local = 42;
    return f(&local);
}

func main() {
    var r = dangle();
    printf("{}\n", r!);
}
