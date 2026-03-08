// Transitive lifetime bounds: 'a: 'b, 'b: 'c.
// 'a must transitively outlive 'c.
// Assigning shortest-lived to 'a violates the chain.
// expect-error: does not live long enough

struct Triple<'a: 'b, 'b: 'c, 'c> {
    first: &'a int;
    second: &'b int;
    third: &'c int;
}

func main() {
    var x = 1;
    var y = 2;
    var z = 3;
    // BAD: first borrows z (declared last), third borrows x (declared first)
    // 'a: 'b: 'c requires first's source to outlive third's source
    var t = Triple{
        first: &z,
        second: &y,
        third: &x
    };
    printf("{}\n", *t.first);
}

