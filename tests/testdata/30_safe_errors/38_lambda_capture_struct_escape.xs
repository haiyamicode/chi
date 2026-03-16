// Lambda captures a struct by-ref and escapes via return.
// expect-error: does not live long enough

struct Wrapper {
    val: int = 0;
}

func exploit() (func () int) {
    var w = Wrapper{val: 42};
    var f = func () int {
        return w.val;
    };
    return f;
}

func main() {
    printf("{}\n", exploit()());
}
