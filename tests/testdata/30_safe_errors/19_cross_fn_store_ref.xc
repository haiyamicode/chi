// Free function storing a ref into a struct field.
// Can't prove r lives long enough for holder's 'this lifetime.

struct RefHolder {
    val: &int = null;
}

func store_ref(holder: &mut RefHolder, r: &int) {
    holder.val = r;
}

func main() {
    var h = RefHolder{};
    {
        var x = 99;
        store_ref(&mut h, &x);
    }
    printf("{}\n", h.val!);
}
