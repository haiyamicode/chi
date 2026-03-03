// Missing a required field (no default, no constructor)
// expect-error: missing field 'x'
struct Vec2 {
    x: int;
    y: int;
}

func main() {
    var v = Vec2{y: 3};
}

