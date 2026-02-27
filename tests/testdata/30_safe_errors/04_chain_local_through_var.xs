// Local flows through intermediate variable into struct — must be rejected
// expect-error: does not live long enough
struct Holder {
    ref: &int;

    func new(r: &'this int) {
        this.ref = r;
    }

    mut func store(r: &'this int) {
        this.ref = r;
    }
}

func bad() Holder {
    var local = 42;
    var r = &local;
    var h = Holder{r};
    return h;
}

func main() {
}

