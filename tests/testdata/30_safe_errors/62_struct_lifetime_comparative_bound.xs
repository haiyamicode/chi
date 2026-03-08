// Struct comparative lifetime bound: 'a must outlive 'b.
// Assigning a shorter-lived source to 'a violates the bound.
// expect-error: does not live long enough

struct Pair<'a: 'b, 'b> {
    long_ref: &'a int;
    short_ref: &'b int;
}

func main() {
    var a = 1;
    var b = 2;
    // BAD: long_ref borrows b (declared after a), short_ref borrows a
    // 'a: 'b requires long_ref's source to outlive short_ref's source
    var p = Pair{long_ref: &b, short_ref: &a};
    printf("{} {}\n", *p.long_ref, *p.short_ref);
}

