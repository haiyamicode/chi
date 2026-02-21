// Store param without 'this into struct field — must be rejected
struct Holder {
    ref: &int = null;

    mut func store(r: &int) {
        this.ref = r;
    }
}
func main() {}
