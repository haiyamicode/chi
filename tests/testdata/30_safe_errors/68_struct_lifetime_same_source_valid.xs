// Struct comparative bound with same source for both fields — valid.
// Both refs borrow the same variable, so 'a trivially outlives 'b.
// expect-error: does not live long enough

struct Pair<'a: 'b, 'b> {
    long_ref: &'a int;
    short_ref: &'b int;
}

func bad() Pair {
    var x = 1;
    // Both fields borrow x — comparative bound is fine, but returning escapes
    return Pair{long_ref: &x, short_ref: &x};
}

func main() {
    var p = bad();
    printf("{}\n", *p.long_ref);
}
