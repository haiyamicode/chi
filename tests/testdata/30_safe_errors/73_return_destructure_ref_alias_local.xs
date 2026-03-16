// Returning a destructured reference through a local alias of a local must still be rejected
// expect-error: does not live long enough
struct Point {
    x: int;
}

func bad() &int {
    var p = Point{x: 42};
    var {&x} = p;
    return x;
}

func main() {}
