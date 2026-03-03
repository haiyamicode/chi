// Store local ref into struct and return — must be rejected
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

func bad() Holder {
    var local = 42;
    var h = Holder{&local};
    return h;
}

func main() {
}

