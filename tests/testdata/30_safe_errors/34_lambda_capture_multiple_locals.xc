// Lambda captures multiple locals and one outlives the lambda variable.
// expect-error: does not live long enough

func main() {
    var a = 10;
    var f: func () int = func () int {
        return 0;
    };
    var b = 20;
    f = func () int {
        return a + b;
    };
    printf("{}\n", f());
}

