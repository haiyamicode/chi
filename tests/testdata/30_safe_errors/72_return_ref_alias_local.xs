// Returning a reference through a local alias of a local must still be rejected
// expect-error: does not live long enough
func bad() &int {
    var local = 42;
    var r = &local;
    return r;
}

func main() {}
