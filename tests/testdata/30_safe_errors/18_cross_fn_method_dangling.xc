// Cross-function dangling reference via method returning ref.
// Holder.get() returns &int tied to 'this, but h dies before r.

struct Holder {
    ref: &int = null;

    mut func store(r: &'this int) {
        this.ref = r;
    }

    func get() &int {
        return this.ref;
    }
}

func main() {
    var r: &int;
    {
        var val = 42;
        var h = Holder{};
        h.store(&val);
        r = h.get();
    }
    printf("{}\n", r!);
}
