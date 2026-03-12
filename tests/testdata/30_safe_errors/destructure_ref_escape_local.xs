// Destructured reference must borrow the original source, so moving the source invalidates it
// expect-error: used after move
struct Point {
    x: int;
    y: int;
}

func main() {
    var p = Point{x: 1, y: 2};
    var {&x} = p;
    var q = move p;
    println(*x);
    println(q.y);
}

