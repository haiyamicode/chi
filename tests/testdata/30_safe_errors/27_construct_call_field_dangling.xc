// Struct literal with a function call producing a dangling ref in a field.

struct Pair {
    a: &int = null;
    b: &int = null;
}

func identity(r: &int) &int {
    return r;
}

func make_pair(x: &int) Pair {
    var local = 999;
    return Pair { a: x, b: identity(&local) };
}

func main() {
    var safe = 1;
    var p = make_pair(&safe);
    printf("a={}, b={}\n", p.a!, p.b!);
}
