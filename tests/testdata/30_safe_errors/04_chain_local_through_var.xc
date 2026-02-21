// Local flows through intermediate variable into struct — must be rejected
struct Holder {
    ref: &int = null;

    mut func store(r: &'this int) {
        this.ref = r;
    }
}

func bad() Holder {
    var h = Holder{};
    var local = 42;
    var r = &local;
    h.store(r);
    return h;
}
func main() {}
