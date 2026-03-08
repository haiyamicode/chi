struct Point {
    x: int = 0;
    y: int = 0;

    func sum() int {
        return this.x + this.y;
    }
}

func test_null_coalescing() {
    println("test_null_coalescing:");

    var a: ?int = 42;
    var result = a ?? 0;
    printf("a ?? 0 = {}\n", result);

    var b: ?int = null;
    result = b ?? 99;
    printf("b ?? 99 = {}\n", result);

    println("");
}

func test_optional_chain_field() {
    println("test_optional_chain_field:");

    var p: ?Point = Point{x: 10, y: 20};
    var x = p?.x;
    printf("p?.x = {}\n", x);

    var q: ?Point = null;
    var y = q?.y;
    printf("q?.y = {}\n", y);

    println("");
}

func test_optional_chain_method() {
    println("test_optional_chain_method:");

    var p: ?Point = Point{x: 3, y: 4};
    var s = p?.sum();
    printf("p?.sum() = {}\n", s);

    var q: ?Point = null;
    var s2 = q?.sum();
    printf("q?.sum() = {}\n", s2);

    println("");
}

func test_chaining() {
    println("test_chaining:");

    var p: ?Point = Point{x: 100, y: 200};
    var x = p?.x ?? -1;
    printf("p?.x ?? -1 = {}\n", x);

    var q: ?Point = null;
    var y = q?.y ?? -1;
    printf("q?.y ?? -1 = {}\n", y);

    println("");
}

func test_null_comparison() {
    println("test_null_comparison:");

    var a: ?int = 42;
    var b: ?int = null;

    // ?T != null
    if a != null {
        printf("a != null: true\n");
    }

    // ?T == null
    if b == null {
        printf("b == null: true\n");
    }

    // null == ?T (reversed)
    if null == b {
        printf("null == b: true\n");
    }

    // null != ?T (reversed)
    if null != a {
        printf("null != a: true\n");
    }

    // ?Struct == null
    var p: ?Point = Point{x: 1, y: 2};
    var q: ?Point = null;
    if p != null {
        printf("p != null: true\n");
    }
    if q == null {
        printf("q == null: true\n");
    }

    println("");
}

func check_bool(name: string, opt: ?bool) {
    if opt {
        printf("{}: truthy\n", name);
    } else {
        printf("{}: falsy\n", name);
    }
}

func check_int(name: string, opt: ?int) {
    if opt {
        printf("{}: truthy\n", name);
    } else {
        printf("{}: falsy\n", name);
    }
}

func test_optional_truthiness() {
    println("test_optional_truthiness:");

    check_bool("null", null);
    check_bool("?false", false);
    check_bool("?true", true);

    check_int("null", null);
    check_int("?0", 0);
    check_int("?42", 42);

    // ?Struct: has_value only (struct is not boolish)
    var p: ?Point = Point{x: 0, y: 0};
    var q: ?Point = null;
    if p {
        printf("?Point(0,0): truthy\n");
    }
    if q {
        printf("?null Point: truthy\n");
    } else {
        printf("?null Point: falsy\n");
    }

    println("");
}

func main() {
    test_null_coalescing();
    test_optional_chain_field();
    test_optional_chain_method();
    test_chaining();
    test_null_comparison();
    test_optional_truthiness();
}

