// Struct literal with a function call producing a dangling ref in a field.
// expect-error: does not live long enough

struct Pair {
    a: &int;
    b: &int;

    func new(a: &'this int, b: &'this int) {
        this.a = a;
        this.b = b;
    }
}

func identity(r: &int) &int {
    return r;
}

func make_pair(x: &int) Pair {
    var local = 999;
    return Pair{x, identity(&local)};
}

func main() {
    var safe = 1;
    var p = make_pair(&safe);
    printf("a={}, b={}\n", p.a!, p.b!);
}
