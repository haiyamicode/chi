// Test malformed operator overloading
import "std/ops" as ops;

struct Point implements ops.Add {
    x: int;
    y: int;

    // Missing function body
    func add(other: Point) Point

    // Incomplete function
    func subtract(

struct Broken implements ops. {
    // Incomplete interface
}

struct Missing implements {
    // Missing interface name
    value: int;
}

func main() {
    var p1: Point = {1, 2;
    var p2 = p1 +++ p2;  // Invalid operator
    var p3 = p1 + ;      // Missing operand
}
