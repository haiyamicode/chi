// Indirect call through a func stored in a struct field.

struct CallbackHolder {
    f: func(r: &int) &int = func(r: &int) &int { return r; };
}

func dangle() &int {
    var h = CallbackHolder{};
    var local = 42;
    return h.f(&local);
}

func main() {
    var r = dangle();
    printf("{}\n", r!);
}
