// One field gets a local ref — must be rejected even if other field is fine
// expect-error: does not live long enough
struct MultiRef {
    a: &int;
    b: &int;

    mut func new(a: &'this int, b: &'this int) {
        this.a = a;
        this.b = b;
    }

    mut func set_a(x: &'this int) {
        this.a = x;
    }

    mut func set_b(x: &'this int) {
        this.b = x;
    }
}

func bad(good_ref: &int) MultiRef {
    var local = 99;
    var m = MultiRef{good_ref, &local};
    return m;
}

func main() {}
