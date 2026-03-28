// Raw pointer member access outside unsafe block.
// expect-error: raw pointer member access requires unsafe block

struct Point {
    value: int = 0;
}

func make_ptr(point: &Point) *Point {
    unsafe {
        return point as *Point;
    }
}

func main() {
    var point = Point{value: 1};
    var p = make_ptr(&point);
    println(p.value);
}
