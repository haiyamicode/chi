// Lambda escapes through a wrapper function that stores it in a struct.
// expect-error: does not live long enough

struct Holder {
    f: func () int = func () int {
        return 0;
    };
}

func wrap(f: func () int) Holder {
    return {:f};
}

func exploit() Holder {
    var x = 42;
    return wrap(func () int {
        return x;
    });
}

func main() {
    var h = exploit();
    printf("{}\n", h.f());
}

