// Cross-function dangling reference via ref-returning free function.
// get_ref returns &int tied to h's lifetime, but h dies before r.
// expect-error: does not live long enough

struct RefHolder {
    val: &int;

    mut func new(v: &'this int) {
        this.val = v;
    }
}

func get_ref(h: &RefHolder) &int {
    return h.val;
}

func main() {
    var r: &int;
    {
        var x = 99;
        var h = RefHolder{&x};
        r = get_ref(&h);
    }
    printf("{}\n", *r);
}

