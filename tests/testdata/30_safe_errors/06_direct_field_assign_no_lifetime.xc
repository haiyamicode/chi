// Direct field assignment in method without 'this — must be rejected
// expect-error: does not live long enough
struct Container {
    val: &int;

    func new(v: &'this int) {
        this.val = v;
    }

    mut func set(v: &int) {
        this.val = v;
    }
}
func main() {}
