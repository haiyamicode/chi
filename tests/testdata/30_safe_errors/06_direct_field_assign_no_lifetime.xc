// Direct field assignment in method without 'this — must be rejected
struct Container {
    val: &int = null;

    mut func set(v: &int) {
        this.val = v;
    }
}
func main() {}
