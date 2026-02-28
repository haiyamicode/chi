// Tests that typed construct expressions (T{}) are valid in all expression positions

struct Point {
    x: int = 0;
    y: int = 0;

    mut func new(x: int, y: int) {
        this.x = x;
        this.y = y;
    }
}

func use_point(p: Point) {
    printf("{} {}\n", p.x, p.y);
}

func main() {
    // T{} as a direct function argument
    use_point(Point{1, 2});

    // T{} in if-expr arrow branches
    var flag = true;
    use_point(if flag => Point{3, 4} else => Point{5, 6});
    use_point(if !flag => Point{3, 4} else => Point{5, 6});

    // T{} in switch-expr arrow branch
    var n = 1;
    use_point(switch n { 1 => Point{9, 10}, else => Point{11, 12} });

    // T{} in array literal elements
    var pts = [Point{13, 14}, Point{15, 16}];
    use_point(pts[0]);
    use_point(pts[1]);

    // T{} in arrow lambda body
    var make: func () Point = () => Point{17, 18};
    use_point(make());
}
