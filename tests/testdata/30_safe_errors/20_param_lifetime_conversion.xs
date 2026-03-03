// Storing one param's ref into another param's struct field.
// Can't prove r's lifetime satisfies holder's 'this lifetime.
// expect-error: does not live long enough

struct RefHolder {
    val: &int;

    mut func new(v: &'this int) {
        this.val = v;
    }
}

func store_into(holder: &mut RefHolder, r: &int) {
    holder.val = r;
}

func main() {
    var x = 10;
    var h = RefHolder{&x};
    store_into(&mut h, &x);
}

