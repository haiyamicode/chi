// Store local ref into struct and return — must be rejected
struct Holder {
    ref: &int = null;

    mut func store(r: &'This int) {
        this.ref = r;
    }
}

func bad() Holder {
    var h = Holder{};
    var local = 42;
    h.store(&local);
    return h;
}
func main() {}
