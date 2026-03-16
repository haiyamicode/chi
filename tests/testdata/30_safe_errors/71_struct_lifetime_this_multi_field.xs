// Implicit 'this bound with multiple ref fields.
// One field's source declared after the struct — must be rejected.
// expect-error: does not live long enough

struct TwoRefs<'a, 'b> {
    first: &'a int;
    second: &'b int;
}

func main() {
    var a = 1;
    var t: TwoRefs;
    var b = 2;
    t = {first: &a, second: &b};
    printf("{} {}\n", *t.first, *t.second);
}
