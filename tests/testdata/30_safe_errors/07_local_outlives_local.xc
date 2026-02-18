// h declared before local — h outlives local (LIFO), dangling ref during destruction
struct Holder {
    ref: &int = null;

    mut func store(r: &'This int) {
        this.ref = r;
    }
}

func bad() {
    var h = Holder{};
    var local = 42;
    h.store(&local);
}
func main() {}
