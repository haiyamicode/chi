// Lambda assigned to variable declared before the captured local — LIFO violation.
// expect-error: does not live long enough

func main() {
    var f: func () int = func () int {
        return 0;
    };
    var local = 42;
    f = func () int {
        return local;
    };
    printf("{}\n", f());
}
