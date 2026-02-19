// Cross-function dangling reference via ref-returning free function.
// get_ref returns &int tied to h's lifetime, but h dies before r.

struct RefHolder {
    val: &int = null;
}

func get_ref(h: &RefHolder) &int {
    return h.val;
}

func main() {
    var r: &int;
    {
        var x = 99;
        var h = RefHolder{val: &x};
        r = get_ref(&h);
    }
    printf("{}\n", r!);
}
