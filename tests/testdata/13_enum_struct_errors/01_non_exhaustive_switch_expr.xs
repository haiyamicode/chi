// Non-exhaustive switch expression must have an else clause
// expect-error: non-exhaustive switch expression must have an else clause
enum Color {
    Red,
    Green,
    Blue
}

func main() {
    var c = Color.Green;
    var name = switch c {
        Color.Red => "red",
        Color.Green => "green"
    };
    println(name);
}
