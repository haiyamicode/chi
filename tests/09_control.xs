func switch_int() string {
    var levels: Array<int> = [];
    levels.push(1);
    levels.push(2);
    levels.push(3);
    levels.push(4);

    for item in levels {
        var label = switch item {
            1, 2 => "low",
            3 => "medium",
            else => {
                println("default case:");
                "high"
            }
        };
        println(label);
    }

    return switch levels[0] {
        1, 2 => "low",
        3 => "medium",
        else => "high"
    };
}

func test_if_expr() {
    println("=== if expression ===");

    // basic if expression
    var x = if true => 1 else => 2;
    println(x);

    // if-else-if chain
    var n = 15;
    var s = if n > 20 => "big" else if n > 10 => "medium" else => "small";
    println(s);

    // arrow syntax
    var a = if true => 10 else => 20;
    println(a);

    // arrow with else-if
    var b = if false => "x" else if true => "y" else => "z";
    println(b);

    // in function argument
    printf("val: {}\n", if 1 > 0 => "yes" else => "no");

    // nested
    var c = if true => if false => 1 else => 2 else => 3;
    println(c);
}

func main() {
    var result = switch_int();
    printf("result for first level: {}\n", result);
    test_if_expr();
}
