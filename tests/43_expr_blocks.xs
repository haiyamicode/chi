// Test that if and switch are correctly treated as expressions vs statements

func if_expr(x: int) int {
    // if-else as expression (both branches return a value)
    var result = if x > 0 => 1 else => -1;
    return result;
}

func if_stmt(x: int) {
    // if-else as statement (no return values)
    if x > 0 {
        println("positive");
    } else {
        println("non-positive");
    }
}

func if_stmt_no_else(x: int) {
    // if without else is always a statement
    if x > 0 {
        println("positive");
    }
}

func switch_expr(x: int) string {
    // switch as expression (cases return values)
    var result = switch x {
        1 => "one",
        2 => "two",
        else => "other"
    };
    return result;
}

func switch_stmt(x: int) {
    // switch as statement (cases don't return values)
    switch x {
        1 => println("one"),
        2 => println("two"),
        else => println("other")
    }
}

func nested_if_expr(x: int) int {
    // nested if-else expression
    return if x > 0 => if x > 10 => 2 else => 1 else => 0;
}

func if_in_unsafe() {
    // void if-else inside unsafe block (was crashing before fix)
    unsafe {
        var x = 5;
        if x > 0 {
            printf("x={}\n", x);
        } else {
            printf("x<=0\n");
        }
    }
}

func switch_in_unsafe(x: int) {
    // void switch inside unsafe block
    unsafe {
        switch x {
            1 => printf("one\n"),
            else => printf("other\n")
        }
    }
}

func main() {
    println("if expressions:");
    printf("if_expr(5)={}\n", if_expr(5));
    printf("if_expr(-3)={}\n", if_expr(-3));

    println("if statements:");
    if_stmt(5);
    if_stmt(-3);
    if_stmt_no_else(5);
    if_stmt_no_else(-3);

    println("switch expressions:");
    printf("switch_expr(1)={}\n", switch_expr(1));
    printf("switch_expr(2)={}\n", switch_expr(2));
    printf("switch_expr(99)={}\n", switch_expr(99));

    println("switch statements:");
    switch_stmt(1);
    switch_stmt(2);
    switch_stmt(99);

    println("nested if expressions:");
    printf("nested_if_expr(20)={}\n", nested_if_expr(20));
    printf("nested_if_expr(5)={}\n", nested_if_expr(5));
    printf("nested_if_expr(-1)={}\n", nested_if_expr(-1));

    println("if/switch in unsafe blocks:");
    if_in_unsafe();
    switch_in_unsafe(1);
    switch_in_unsafe(5);
}

