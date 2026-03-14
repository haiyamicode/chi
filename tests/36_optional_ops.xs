struct Point {
    x: int = 0;
    y: int = 0;

    func sum() int {
        return this.x + this.y;
    }
}

struct PointHolder {
    point: ?Point = null;
}

enum MaybeValue {
    Number(int),
    Coord {
        x: int;
        y: int;
    }
}

struct EnumHolder {
    value: MaybeValue = Number{0};
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

func maybe_point(has_value: bool) ?Point {
    printf("maybe_point({})\n", has_value);
    if has_value {
        return Point{x: 7, y: 9};
    }
    return null;
}

func test_if_let() {
    println("test_if_let:");

    var p: ?Point = Point{x: 10, y: 20};
    if let point = p {
        printf("let point=({}, {})\n", point.x, point.y);
    }

    var q: ?Point = null;
    if let point = q {
        printf("unexpected point=({}, {})\n", point.x, point.y);
    } else {
        println("let null else");
    }

    var sum = if let point = maybe_point(true) => point.x + point.y else => -1;
    printf("if let expr sum={}\n", sum);

    var miss = if let point = maybe_point(false) => point.x + point.y else => -1;
    printf("if let expr miss={}\n", miss);

    var r: ?Point = Point{x: 1, y: 2};
    if var point = r {
        point.x = 11;
        point.y = 22;
    }
    printf("var point mutated=({}, {})\n", r!.x, r!.y);

    var s: ?Point = Point{x: 3, y: 4};
    if let {x, y} = s {
        printf("let destructure=({}, {})\n", x, y);
    }

    var t: ?Point = Point{x: 5, y: 6};
    if var {&mut x, &mut y} = t {
        *x = 15;
        *y = 16;
    }
    printf("var destructure mutated=({}, {})\n", t!.x, t!.y);

    var destructured = if let {x, y} = maybe_point(true) => x + y else => -1;
    printf("if let destructure expr={}\n", destructured);

    var holder = PointHolder{point: Point{x: 21, y: 22}};
    if var point = holder.point {
        point.x = 31;
        point.y = 32;
    }
    printf("field binding mutated=({}, {})\n", holder.point!.x, holder.point!.y);

    if let {x, y} = holder.point {
        printf("field destructure=({}, {})\n", x, y);
    }

    println("");
}

func test_if_let_enum_pattern() {
    println("test_if_let_enum_pattern:");

    var number = MaybeValue.Number{42};
    if let Number(value) = number {
        printf("enum tuple={}\n", value);
    }

    var coord = MaybeValue.Coord{x: 3, y: 4};
    if let Coord{x, y} = coord {
        printf("enum struct=({}, {})\n", x, y);
    }

    var enum_source = MaybeValue.Number{55};
    var enum_sum = if let Number(value) = enum_source => value else => -1;
    printf("enum expr={}\n", enum_sum);

    var enum_source2 = MaybeValue.Coord{x: 1, y: 2};
    var enum_miss = if let Number(value) = enum_source2 => value else => -1;
    printf("enum miss={}\n", enum_miss);

    var coord2 = MaybeValue.Coord{x: 5, y: 6};
    if let Coord{x, y} = coord2 {
        printf("enum second struct=({}, {})\n", x, y);
    }

    var holder = EnumHolder{value: MaybeValue.Coord{x: 7, y: 8}};
    if let Coord{x, y} = holder.value {
        printf("enum field=({}, {})\n", x, y);
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
    test_if_let();
    test_if_let_enum_pattern();
}

