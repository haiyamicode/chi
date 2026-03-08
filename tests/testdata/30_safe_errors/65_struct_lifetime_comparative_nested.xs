// Nested scope: 'a source declared in inner scope, 'b source in outer scope.
// 'a: 'b requires 'a to outlive 'b, but inner-scope variable dies first.
// expect-error: does not live long enough

struct Pair<'a: 'b, 'b> {
    long_ref: &'a int;
    short_ref: &'b int;
}

func use_pair(p: Pair) {
    printf("{} {}\n", *p.long_ref, *p.short_ref);
}

func main() {
    var outer = 10;
    {
        var inner = 20;
        // BAD: long_ref borrows inner (shorter-lived), short_ref borrows outer
        var p = Pair{long_ref: &inner, short_ref: &outer};
        use_pair(p);
    }
}

