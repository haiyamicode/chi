// Lambda captures a local through a reference dereference pattern.
// The ref itself is safe, but the lambda holds the local's address.
// expect-error: does not live long enough

func main() {
    var f: func () int = func () int {
        return 0;
    };
    var local = 42;
    var r: &int = &local;
    f = func () int {
        return *r;
    };
    printf("{}\n", f());
}

