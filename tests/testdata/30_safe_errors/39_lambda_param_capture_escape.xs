// Lambda captures a value parameter by-ref and escapes.
// The parameter dies when the function returns.
// expect-error: does not live long enough

func capture_param(x: int) (func () int) {
    return func () int {
        return x;
    };
}

func main() {
    var f = capture_param(42);
    printf("{}\n", f());
}
