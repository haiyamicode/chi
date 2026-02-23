// Cross-function dangling reference via method returning ref.
// Holder.get() returns &int tied to 'this, but h dies before r.
// expect-error: does not live long enough

struct Holder {
    ref: &int;

    func new(r: &'this int) {
        this.ref = r;
    }

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
        var h = Holder{&val};
        r = h.get();
    }
    printf("{}\n", r!);
}

