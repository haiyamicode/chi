// Lambda variable declared early, then reassigned in inner scope to capture
// a shorter-lived local — LIFO violation through scope nesting.
// expect-error: does not live long enough

func main() {
    var f: func() int = func() int { return 0; };
    {
        var inner_val = 99;
        f = func() int { return inner_val; };
    }
    printf("{}\n", f());
}
