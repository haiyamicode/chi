// Return reference to local variable — must be rejected
// expect-error: does not live long enough
func bad() &int {
    var local = 42;
    return &local;
}

func main() {
}

