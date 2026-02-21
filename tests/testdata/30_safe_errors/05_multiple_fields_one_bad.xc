// One field gets a local ref — must be rejected even if other field is fine
struct MultiRef {
    a: &int = null;
    b: &int = null;

    mut func set_a(x: &'this int) {
        this.a = x;
    }

    mut func set_b(x: &'this int) {
        this.b = x;
    }
}

func bad(good_ref: &int) MultiRef {
    var m = MultiRef{};
    var local = 99;
    m.set_a(good_ref);
    m.set_b(&local);
    return m;
}
func main() {}
