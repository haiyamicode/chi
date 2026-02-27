// Store param without 'this into struct field — must be rejected
// expect-error: does not live long enough
struct Holder {
    ref: &int;

    func new(r: &'this int) {
        this.ref = r;
    }

    mut func store(r: &int) {
        this.ref = r;
    }
}

func main() {
}

