// Test malformed type symbol access and member chains
enum Color {
    Red,
    Green,
    Blue

struct Point {
    x: int;
    y: int;

    static func origin() Point {
        return {0, 0
    }
}

func main() {
    // Incomplete enum access
    var c = Color.

    // Chained static calls with errors
    var p1 = Point.origin().

    // Invalid type as expression
    var t = Point;

    // Member access on type name (non-static)
    var x = Point.x;

    // Deep incomplete chain
    var p2 = Point.origin().x.

    // Malformed construct with type symbol
    var p3 = Point{.x =

    // Invalid static method on builtin type
    var i = int.format();

    // Incomplete member after valid access
    "hello".is_empty().
}
