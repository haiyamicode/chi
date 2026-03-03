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

// === Field-init construction (no constructor) ===

// All fields required (no defaults)
struct Vec2 {
    x: int;
    y: int;
}

// Mix of required and defaulted fields
struct Config {
    name: string;
    width: int;
    height: int = 600;
    visible: bool = true;
}

// Nested struct without constructor
struct Line {
    start: Vec2;
    end_pt: Vec2;
}

func use_vec2(v: Vec2) {
    printf("{} {}\n", v.x, v.y);
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

    // === Field-init construction tests ===
    println("--- field init ---");

    // All required fields provided
    var v = Vec2{x: 10, y: 20};
    printf("{} {}\n", v.x, v.y);

    // Mixed: required + defaulted (only required provided)
    var c = Config{name: "app", width: 800};
    printf("{} {} {} {}\n", c.name, c.width, c.height, c.visible);

    // Mixed: override defaults too
    var c2 = Config{name: "game", width: 1920, height: 1080, visible: false};
    printf("{} {} {} {}\n", c2.name, c2.width, c2.height, c2.visible);

    // Nested structs without constructors
    var line = Line{
        start: Vec2{x: 0, y: 0},
        end_pt: Vec2{x: 100, y: 200},
    };
    printf("{} {} {} {}\n", line.start.x, line.start.y, line.end_pt.x, line.end_pt.y);

    // Field init as function argument
    use_vec2(Vec2{x: 42, y: 99});

    // Field init in array literal
    var vecs = [Vec2{x: 1, y: 2}, Vec2{x: 3, y: 4}];
    printf("{} {} {} {}\n", vecs[0].x, vecs[0].y, vecs[1].x, vecs[1].y);
}
