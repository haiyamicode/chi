// Direct field assignment in method without 'This — must be rejected
struct Container {
    val: &int = null;

    mut func set(v: &int) {
        this.val = v;
    }
}
func main() {}
