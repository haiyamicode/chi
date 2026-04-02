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

struct FlagBox {
    enabled: bool = true;

    func is_enabled() bool {
        return this.enabled;
    }
}

func show_point(name: string, p: Point) {
    printf("{} = ({}, {})\n", name, p.x, p.y);
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

    var fallback_point: ?Point = null;
    show_point("fallback point", fallback_point ?? {});

    var inferred_point: ?Point = null;
    let inferred_fallback = inferred_point ?? {};
    show_point("inferred fallback point", inferred_fallback);

    println("");
}

func test_construct_operand_postfix() {
    println("test_construct_operand_postfix:");

    var maybe_enabled: ?bool = null;
    printf("maybe ?? field = {}\n", maybe_enabled ?? FlagBox{}.enabled);
    printf("true && field = {}\n", true && FlagBox{}.enabled);
    printf("true && method = {}\n", true && FlagBox{}.is_enabled());

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

func test_optional_presence() {
    println("test_optional_presence:");

    check_bool("null", null);
    check_bool("?false", false);
    check_bool("?true", true);

    check_int("null", null);
    check_int("?0", 0);
    check_int("?42", 42);

    // Optionals convert to bool by presence only.
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

func first_segment(path: string) ?string {
    let entries = path.split(":");
    for entry in entries {
        return entry;
    }
    return null;
}

func take_first_segment(path: string) string {
    if let segment = first_segment(path) {
        return segment;
    }
    return "";
}

func take_present(value: ?string) string {
    if value {
        return value;
    }
    return "";
}

func take_present_after_guard(value: ?string) string {
    if !value {
        return "";
    }
    return value;
}

func make_optional_string(value: string) ?string {
    return value;
}

func echo_string(value: string) string {
    return value;
}

func unwrap_temp_to_local() string {
    let value = make_optional_string("local")!;
    return value;
}

func unwrap_temp_to_arg() string {
    return echo_string(make_optional_string("arg")!);
}

func unwrap_temp_to_return() string {
    return make_optional_string("return")!;
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

    var holder = EnumHolder{value: Coord{x: 7, y: 8}};
    if let Coord{x, y} = holder.value {
        printf("enum field=({}, {})\n", x, y);
    }
}

func test_if_let_zero() {
    println("test_if_let_zero:");

    var zero: ?int = 0;
    if let value = zero {
        printf("let zero={}\n", value);
    } else {
        println("unexpected zero else");
    }

    var computed = if let value = zero => value + 10 else => -1;
    printf("if let zero expr={}\n", computed);
    printf("if let alias return={}\n", take_first_segment("a:b:hello:c"));

    println("");
}

func test_implicit_narrow() {
    println("test_implicit_narrow:");

    printf("truthy return={}\n", take_present("hello"));
    printf("guard return={}\n", take_present_after_guard("world"));
    printf("unwrap temp local={}\n", unwrap_temp_to_local());
    printf("unwrap temp arg={}\n", unwrap_temp_to_arg());
    printf("unwrap temp return={}\n", unwrap_temp_to_return());

    println("");
}

func test_optional_inference() {
    println("test_optional_inference:");

    let explicit_if_left: ?Point = true ? (Point{x: 10, y: 11}) : null;
    printf("explicit ?T if T/null = ({}, {})\n", explicit_if_left!.x, explicit_if_left!.y);

    let explicit_if_right: ?Point = false ? null : (Point{x: 12, y: 13});
    printf("explicit ?T if null/T = ({}, {})\n", explicit_if_right!.x, explicit_if_right!.y);

    var inferred_if_left = true ? (Point{x: 1, y: 2}) : null;
    printf("if T/null = ({}, {})\n", inferred_if_left!.x, inferred_if_left!.y);

    var inferred_if_right = false ? null : (Point{x: 3, y: 4});
    printf("if null/T = ({}, {})\n", inferred_if_right!.x, inferred_if_right!.y);

    var inferred_switch_left = switch 1 {
        1 => (Point{x: 5, y: 6}),
        else => null
    };
    printf("switch T/null = ({}, {})\n", inferred_switch_left!.x, inferred_switch_left!.y);

    var inferred_switch_right = switch 0 {
        1 => null,
        else => (Point{x: 7, y: 8})
    };
    printf("switch null/T = ({}, {})\n", inferred_switch_right!.x, inferred_switch_right!.y);

    var nested: ?int = 9;
    var nested2: ??int = nested;
    var nested3: ???int = nested2;
    printf("direct ?T/??T/???T = {} {} {}\n", nested!, nested2!!, nested3!!!);

    var inferred_nested_if = true ? nested : null;
    var inferred_deeper_if = true ? nested2 : null;
    printf(
        "if ?T/null preserves = {} / if ??T/null preserves = {}\n",
        inferred_nested_if!,
        inferred_deeper_if!!
    );

    var inferred_nested_switch = switch 1 {
        1 => nested,
        else => null
    };
    var inferred_deeper_switch = switch 1 {
        1 => nested2,
        else => null
    };
    printf(
        "switch ?T/null preserves = {} / switch ??T/null preserves = {}\n",
        inferred_nested_switch!,
        inferred_deeper_switch!!
    );

    println("");
}

func get_opt_none() ?string {
    return null;
}

func get_opt_some() ?string {
    return "found";
}

func get_opt2_none() ??string {
    return null;
}

func get_opt2_some() ??string {
    return get_opt_some();
}

func get_opt2_inner_null() ??string {
    var x: ?string = null;
    return x;
}

func test_coalesce_chainable() {
    println("test_coalesce_chainable:");

    // ?T ?? ?T -> ?T
    var a = get_opt_none() ?? get_opt_none();
    printf("none ?? none = {}\n", a ?? "nil");
    var b = get_opt_none() ?? get_opt_some();
    printf("none ?? some = {}\n", b ?? "nil");
    var c = get_opt_some() ?? get_opt_none();
    printf("some ?? none = {}\n", c ?? "nil");

    // ?T ?? ?T ?? T -> T
    var d = get_opt_none() ?? get_opt_none() ?? "default";
    printf("none ?? none ?? default = {}\n", d);

    // ??T ?? ??T -> ??T
    var e = get_opt2_none() ?? get_opt2_none();
    printf("??none ?? ??none is null = {}\n", e == null);
    var f = get_opt2_none() ?? get_opt2_some();
    if let f_val = f {
        printf("??none ?? ??some = {}\n", f_val ?? "inner_nil");
    } else {
        println("??none ?? ??some = null");
    }

    // ??T ?? ?T -> ?T (unwrap one level)
    var g = get_opt2_none() ?? get_opt_some();
    printf("??none ?? ?some = {}\n", g ?? "nil");
    var h = get_opt2_some() ?? get_opt_some();
    printf("??some ?? ?some = {}\n", h ?? "nil");

    // ??T with inner null ?? ?T -> ?T
    var i = get_opt2_inner_null() ?? get_opt_some();
    printf("??inner_null ?? ?some = {}\n", i ?? "nil");

    // ???T chaining
    var x: ???string = null;
    var y: ???string = get_opt2_some();
    var z = x ?? y;
    printf("???null ?? ???some is null = {}\n", z == null);
    var w = x ?? y ?? get_opt2_some();
    printf("???null ?? ???some ?? ??some is null = {}\n", w == null);

    println("");
}

func main() {
    test_null_coalescing();
    test_construct_operand_postfix();
    test_optional_chain_field();
    test_optional_chain_method();
    test_chaining();
    test_null_comparison();
    test_optional_presence();
    test_if_let();
    test_if_let_zero();
    test_implicit_narrow();
    test_optional_inference();
    test_if_let_enum_pattern();
    test_coalesce_chainable();
}
