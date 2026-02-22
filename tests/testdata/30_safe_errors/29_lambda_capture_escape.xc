// Lambda captures local by-ref and escapes the function — dangling capture.
// expect-error: does not live long enough

func make_fn() func() int {
    var secret = 12345;
    return func() int { return secret; };
}

func main() {
    printf("{}\n", make_fn()());
}
