import "std/ops" as ops;

// === Struct definitions ===

struct Point {
    x: int = 0;
    y: int = 0;
}

struct Named {
    name: string = "";
    value: int = 0;
}

struct Wrapper {
    inner: Point = {};
    label: string = "";
}

struct Deep {
    wrapper: Wrapper = {};
    id: int = 0;
}

// Traced type for lifecycle verification (silent when id=0 to avoid default-construction noise)
struct Traced {
    id: int = 0;

    mut func new(id: int = 0) {
        this.id = id;
        if id != 0 {
            printf("Traced({}) created\n", id);
        }
    }

    mut func delete() {
        if this.id != 0 {
            printf("Traced({}) destroyed\n", this.id);
        }
    }

    impl ops.CopyFrom<Traced> {
        mut func copy_from(source: &Traced) {
            this.id = source.id;
            if source.id != 0 {
                printf("Traced({}) copied\n", source.id);
            }
        }
    }
}

struct TracedPair {
    a: Traced = {};
    b: Traced = {};
}

struct TracedWrapper {
    inner: TracedPair = {};
    label: string = "";
}

// Cross-type spread structs
struct Base {
    x: int = 0;
    name: string = "";
}

struct Extended {
    x: int = 0;
    name: string = "";
    y: int = 0;
    z: int = 0;
}

struct TracedBase {
    t: Traced = {};
    name: string = "";
}

struct TracedExtended {
    t: Traced = {};
    name: string = "";
    extra: int = 0;
}

// === Basic destructuring ===

func test_basic() {
    var p = Point{x: 1, y: 2};
    var {x, y} = p;
    printf("x={} y={}\n", x, y);
}

// === Destructuring with rename ===

func test_rename() {
    var p = Point{x: 10, y: 20};
    let {x: a, y: b} = p;
    printf("a={} b={}\n", a, b);
}

// === Nested destructuring ===

func test_nested() {
    var w = Wrapper{inner: Point{x: 3, y: 4}, label: "hello"};
    var {inner: {x, y}, label} = w;
    printf("x={} y={} label={}\n", x, y, label);
}

// === Deep nested destructuring ===

func test_deep_nested() {
    var d = Deep{wrapper: Wrapper{inner: Point{x: 5, y: 6}, label: "deep"}, id: 42};
    var {wrapper: {inner: {x, y}, label}, id} = d;
    printf("x={} y={} label={} id={}\n", x, y, label, id);
}

// === Spread with no overrides ===

func test_spread_copy() {
    var p = Point{x: 1, y: 2};
    var p2 = Point{...p};
    printf("p2.x={} p2.y={}\n", p2.x, p2.y);
}

// === Spread with field override ===

func test_spread_override() {
    var p = Point{x: 1, y: 2};
    var p2 = Point{...p, x: 10};
    printf("p2.x={} p2.y={}\n", p2.x, p2.y);
}

// === Spread with string field ===

func test_spread_string() {
    var n = Named{name: "hello", value: 42};
    var n2 = Named{...n, value: 99};
    printf("n2.name={} n2.value={}\n", n2.name, n2.value);
    // Original should still be valid
    printf("n.name={} n.value={}\n", n.name, n.value);
}

// === Spread all fields overridden ===

func test_spread_all_override() {
    var p = Point{x: 1, y: 2};
    var p2 = Point{
        ...p,
        x: 100,
        y: 200
    };
    printf("p2.x={} p2.y={}\n", p2.x, p2.y);
}

// === Destructuring with string field ===

func test_destructure_string() {
    var n = Named{name: "world", value: 7};
    var {name, value} = n;
    printf("name={} value={}\n", name, value);
    // Original still valid
    printf("n.name={} n.value={}\n", n.name, n.value);
}

// === Let destructuring (immutable) ===

func test_let_destructure() {
    var p = Point{x: 50, y: 60};
    let {x, y} = p;
    printf("x={} y={}\n", x, y);
}

// === Lifecycle: destructure from variable (scoped) ===

func test_destructure_lifecycle() {
    var pair = TracedPair{a: Traced{1}, b: Traced{2}};
    println("before destructure");
    {
        var {a, b} = pair;
        printf("a.id={} b.id={}\n", a.id, b.id);
        println("block exit");
    }
    println("after block");
    printf("pair.a.id={} pair.b.id={}\n", pair.a.id, pair.b.id);
    println("func exit");
}

// === Lifecycle: spread with traced fields ===

func test_spread_lifecycle() {
    var p1 = TracedPair{a: Traced{10}, b: Traced{20}};
    println("before spread");
    var p2 = TracedPair{...p1, b: Traced{30}};
    printf("p1.a.id={} p1.b.id={}\n", p1.a.id, p1.b.id);
    printf("p2.a.id={} p2.b.id={}\n", p2.a.id, p2.b.id);
    println("func exit");
}

// === Lifecycle: nested destructure with traced types ===

func test_nested_destructure_lifecycle() {
    var w = TracedWrapper{inner: TracedPair{a: Traced{100}, b: Traced{200}}, label: "test"};
    println("before destructure");
    var {inner: {a, b}, label} = w;
    printf("a.id={} b.id={}\n", a.id, b.id);
    printf("label={}\n", label);
    println("func exit");
}

// === Lifecycle: destructure from construct expr (RVO) ===

func test_destructure_from_temp() {
    var {a, b} = TracedPair{a: Traced{40}, b: Traced{50}};
    printf("a.id={} b.id={}\n", a.id, b.id);
    println("func exit");
}

// === Lifecycle: spread full copy (no overrides) ===

func test_spread_full_copy() {
    var p1 = TracedPair{a: Traced{60}, b: Traced{70}};
    println("before spread");
    var p2 = TracedPair{...p1};
    printf("p1.a.id={} p1.b.id={}\n", p1.a.id, p1.b.id);
    printf("p2.a.id={} p2.b.id={}\n", p2.a.id, p2.b.id);
    println("func exit");
}

// === Cross-type spread: subset into superset ===

func test_cross_spread_basic() {
    var b = Base{x: 42, name: "hello"};
    var e = Extended{
        ...b,
        y: 10,
        z: 20
    };
    printf("e.x={} e.name={} e.y={} e.z={}\n", e.x, e.name, e.y, e.z);
    // Original still valid
    printf("b.x={} b.name={}\n", b.x, b.name);
}

// === Cross-type spread: subset, extra fields get defaults ===

func test_cross_spread_defaults() {
    var b = Base{x: 7, name: "world"};
    var e = Extended{...b};
    printf("e.x={} e.name={} e.y={} e.z={}\n", e.x, e.name, e.y, e.z);
}

// === Cross-type spread: with override of shared field ===

func test_cross_spread_override() {
    var b = Base{x: 1, name: "old"};
    var e = Extended{
        ...b,
        name: "new",
        y: 5,
        z: 6
    };
    printf("e.x={} e.name={} e.y={} e.z={}\n", e.x, e.name, e.y, e.z);
    printf("b.name={}\n", b.name);
}

// === Cross-type spread: with traced fields (lifecycle) ===

func test_cross_spread_lifecycle() {
    var tb = TracedBase{t: Traced{99}, name: "base"};
    println("before cross spread");
    var te = TracedExtended{...tb, extra: 42};
    printf("te.t.id={} te.name={} te.extra={}\n", te.t.id, te.name, te.extra);
    printf("tb.t.id={} tb.name={}\n", tb.t.id, tb.name);
    println("func exit");
}

// === Cross-type spread: superset into subset (discard extra) ===

func test_cross_spread_discard() {
    var e = Extended{
        x: 1,
        name: "hello",
        y: 10,
        z: 20
    };
    var b = Base{...e};
    printf("b.x={} b.name={}\n", b.x, b.name);
    // Original still valid
    printf("e.x={} e.name={} e.y={} e.z={}\n", e.x, e.name, e.y, e.z);
}

// === Cross-type spread: superset into subset with override ===

func test_cross_spread_discard_override() {
    var e = Extended{
        x: 1,
        name: "old",
        y: 10,
        z: 20
    };
    var b = Base{...e, name: "new"};
    printf("b.x={} b.name={}\n", b.x, b.name);
}

// === Cross-type spread: superset into subset with lifecycle ===

func test_cross_spread_discard_lifecycle() {
    var te = TracedExtended{
        t: Traced{77},
        name: "big",
        extra: 42
    };
    println("before spread");
    var tb = TracedBase{...te};
    printf("tb.t.id={} tb.name={}\n", tb.t.id, tb.name);
    printf("te.t.id={} te.name={} te.extra={}\n", te.t.id, te.name, te.extra);
    println("func exit");
}

// === Array destructuring: basic ===

func test_array_basic() {
    var arr: Array<int> = [10, 20, 30];
    var [a, b, c] = arr;
    printf("a={} b={} c={}\n", a, b, c);
}

// === Array destructuring: let ===

func test_array_let() {
    var arr: Array<int> = [10, 20, 30];
    let [x, y] = arr;
    printf("x={} y={}\n", x, y);
}

// === Array destructuring: strings ===

func test_array_strings() {
    var names: Array<string> = ["hello", "world"];
    var [s1, s2] = names;
    printf("s1={} s2={}\n", s1, s2);
    // Original still valid
    printf("names[0]={} names[1]={}\n", names[0], names[1]);
}

func test_ref_struct() {
    var p = Point{x: 10, y: 20};
    var {&x, &y} = p;
    printf("x={} y={}\n", *x, *y);
}

func test_ref_mixed() {
    var p = Point{x: 30, y: 40};
    var {&x: rx, y: cy} = p;
    printf("rx={} cy={}\n", *rx, cy);
}

func test_ref_array() {
    var arr: Array<int> = [100, 200, 300];
    var [&a, &b, c] = arr;
    printf("a={} b={} c={}\n", *a, *b, c);
}

func test_mut_ref_struct() {
    var p = Point{x: 1, y: 2};
    var {&mut x: rx, &mut y: ry} = p;
    printf("rx={} ry={}\n", *rx, *ry);
    *rx = 99;
    *ry = 88;
    printf("rx={} ry={}\n", *rx, *ry);
}

func test_mut_ref_array() {
    var arr: Array<int> = [1, 2, 3];
    var [&mut a, b] = arr;
    printf("a={} b={}\n", *a, b);
    *a = 999;
    printf("a={}\n", *a);
}

func main() {
    println("-- Basic destructuring --");
    test_basic();

    println("\n-- Rename --");
    test_rename();

    println("\n-- Nested --");
    test_nested();

    println("\n-- Deep nested --");
    test_deep_nested();

    println("\n-- Spread copy --");
    test_spread_copy();

    println("\n-- Spread override --");
    test_spread_override();

    println("\n-- Spread string --");
    test_spread_string();

    println("\n-- Spread all override --");
    test_spread_all_override();

    println("\n-- Destructure string --");
    test_destructure_string();

    println("\n-- Let destructure --");
    test_let_destructure();

    println("\n-- Lifecycle: destructure from var --");
    test_destructure_lifecycle();

    println("\n-- Lifecycle: spread --");
    test_spread_lifecycle();

    println("\n-- Lifecycle: nested destructure --");
    test_nested_destructure_lifecycle();

    println("\n-- Lifecycle: destructure from temp --");
    test_destructure_from_temp();

    println("\n-- Lifecycle: spread full copy --");
    test_spread_full_copy();

    println("\n-- Cross spread: basic --");
    test_cross_spread_basic();

    println("\n-- Cross spread: defaults --");
    test_cross_spread_defaults();

    println("\n-- Cross spread: override --");
    test_cross_spread_override();

    println("\n-- Cross spread: lifecycle --");
    test_cross_spread_lifecycle();

    println("\n-- Cross spread: discard --");
    test_cross_spread_discard();

    println("\n-- Cross spread: discard override --");
    test_cross_spread_discard_override();

    println("\n-- Cross spread: discard lifecycle --");
    test_cross_spread_discard_lifecycle();

    println("\n-- Array destructure: basic --");
    test_array_basic();

    println("\n-- Array destructure: let --");
    test_array_let();

    println("\n-- Array destructure: strings --");
    test_array_strings();

    println("\n-- Ref destructure: struct --");
    test_ref_struct();

    println("\n-- Ref destructure: mixed --");
    test_ref_mixed();

    println("\n-- Ref destructure: array --");
    test_ref_array();

    println("\n-- Mut ref destructure: struct --");
    test_mut_ref_struct();

    println("\n-- Mut ref destructure: array --");
    test_mut_ref_array();
}

