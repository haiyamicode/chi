// Free function storing a ref into a struct field.
// Can't prove r lives long enough for holder's 'this lifetime.
// expect-error: does not live long enough

struct RefHolder {
    val: &int;

    mut func new(v: &'this int) {
        this.val = v;
    }
}

func store_ref(holder: &mut RefHolder, r: &int) {
    holder.val = r;
}

func main() {
    var safe = 0;
    var h = RefHolder{&safe};
    {
        var x = 99;
        store_ref(&mut h, &x);
    }
    printf("{}\n", *h.val);
}
