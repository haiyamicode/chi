// import/export wrapping tests
// Short import - should stay inline
import {Foo, Bar, Baz} from "./short";

// Long import - should wrap (>120 chars)
import {VeryLongIdentifierName1, VeryLongIdentifierName2, VeryLongIdentifierName3, VeryLongIdentifierName4, VeryLongIdentifierName5} from "./long";

func apply(x: int, f: func (n: int) int) int {
    return f(x);
}

func combine(a: int, b: int, f: func (x: int, y: int) int) int {
    return f(a, b);
}

func main() {
    // if/while/switch paren collapsing
    var x = 5;
    if (x > 0) {
        println("positive");
    }
    if (x > 0) {
        println("big");
    } else {
        println("small");
    }
    while (x > 0) {
        x = x - 1;
    }

    // arrow lambda func collapsing
    var a = apply(5, func (n) => n * 2);
    var b = combine(1, 2, func (x, y) => x + y);
    var c = apply(3, func (n: int) => n + 1);

    // arrow block collapses to func block
    var d = apply(5, (n) => {
        return n * 2;
    });
    var e = combine(1, 2, func (x, y) => {
        return x + y;
    });

    // typed construct expr collapsing
    var nums = Array<int>{1, 2, 3};

    // redundant construct type in return
    var f = make_point(1, 2);
}

struct Point2 {
    x: int;
    y: int;

    func new(x: int, y: int) {
        this.x = x;
        this.y = y;
    }
}

func make_point(x: int, y: int) Point2 {
    return Point2{x, y};
}
