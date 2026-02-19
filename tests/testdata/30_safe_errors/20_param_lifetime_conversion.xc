// Storing one param's ref into another param's struct field.
// Can't prove r's lifetime satisfies holder's 'This lifetime.

struct RefHolder {
    val: &int = null;
}

func store_into(holder: &mut RefHolder, r: &int) {
    holder.val = r;
}

func main() {
    var x = 10;
    var h = RefHolder{};
    store_into(&mut h, &x);
}
