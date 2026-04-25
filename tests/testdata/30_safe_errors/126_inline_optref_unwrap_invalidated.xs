// Inline `!` unwrap of `?(&T)` must propagate the borrow source so a later
// mut call on the receiver invalidates the alias.
// expect-error: exclusive access

struct InlineRefSource {
    val: int = 0;

    func find() ?(&int) {
        if this.val > 0 {
            return {&this.val};
        }
        return null;
    }

    mut func clobber() {
        this.val = 999;
    }
}

func main() {
    var s = InlineRefSource{val: 7};
    let r: &int = s.find()!;
    s.clobber();
    printf("after: {}\n", *r);
}
