// Comparing enum structs (variants with data) is not allowed.
// expect-error: invalid operator '=='

enum Shape {
    Circle {
        radius: int;
    },
    Rect {
        w: int;
        h: int;
    }
}

func main() {
    var a = Shape.Circle{radius: 5};
    var b = Shape.Circle{radius: 5};
    if a == b {
        println("equal");
    }
}

