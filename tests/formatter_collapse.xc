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
}
