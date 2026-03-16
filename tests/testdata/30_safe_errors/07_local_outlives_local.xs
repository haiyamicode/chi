// h declared before local — h outlives local (LIFO), dangling ref during destruction
// expect-error: does not live long enough
struct Holder {
    ref: &int;

    mut func new(r: &'this int) {
        this.ref = r;
    }

    mut func store(r: &'this int) {
        this.ref = r;
    }
}

func bad() {
    var dummy = 0;
    var h = Holder{&dummy};
    var local = 42;
    h.store(&local);
}

func main() {}
