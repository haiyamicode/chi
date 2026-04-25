// Inline `??` coalesce on `?(&T)` must propagate the borrow source from both
// branches so a later mut call on the receiver invalidates the alias.
// expect-error: exclusive access

struct CoalesceRefSource {
    val: int = 0;
    fallback: int = 999;

    func find() ?(&int) {
        if this.val > 0 {
            return {&this.val};
        }
        return null;
    }

    mut func clobber() {
        this.val = 42;
    }
}

func main() {
    var s = CoalesceRefSource{val: 7};
    let r: &int = s.find() ?? &s.fallback;
    s.clobber();
    printf("after: {}\n", *r);
}
