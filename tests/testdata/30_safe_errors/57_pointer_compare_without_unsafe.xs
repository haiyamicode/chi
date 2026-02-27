// expect-error: pointer comparison requires unsafe block
func main() {
    var x: int = 1;
    var y: int = 2;
    var p1: *int = &x;
    var p2: *int = &y;
    var result = p1 < p2;
}

