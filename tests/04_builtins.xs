import "std/mem" as mem;
import "std/ops" as ops;

func guard_add(x: ?int) int {
    if !x {
        return -1;
    }
    return x + 1;
}

func assert_add(x: ?int) int {
    assert(x);
    return x + 1;
}

func test_optional() {
    println("testing optional:");
    var t: ?int = null;
    if !t {
        println("t is null");
    }
    t! = 5;
    if t {
        println("has value");
        printf("t={}\n", t);
    }
    printf("guard_add(10)={}\n", guard_add(10));
    printf("guard_add(null)={}\n", guard_add(null));
    printf("assert_add(10)={}\n", assert_add(10));
    println("");
}

func test_array() {
    println("testing array:");
    var a: Array<int> = [];
    a.push(1);
    a.push(2);
    printf("a=[{}, {}]\n", a[0], a[1]);
    a[0] = 2;
    a[1] = 1;
    printf("a=[{}, {}]\n", a[0], a[1]);
    var b = [10, 20, 30];
    printf("b=[{}, {}, {}]\n", b[0], b[1], b[2]);
    println("");
}

func test_map() {
    println("testing map:");
    var m = Map<string, int>{};
    m.set("abc", 1);
    m.set("d", 2);
    printf("m[\"abc\"] = {}\n", m["abc"]);
    printf("m[\"d\"] = {}\n", m["d"]);
    printf("m.get(\"ef\") = {}\n", m.get("ef"));
    var it1 = m.get("abc");
    printf("m.get(\"abc\")! = {}\n", it1!);
    var it2 = m.get("invalid");
    printf("m.get(\"invalid\") = {}\n", it2);
    // test keys
    var ks = m.keys();
    printf("keys.length = {}\n", ks.length);
    // test overwrite
    m.set("abc", 99);
    printf("m[\"abc\"] after overwrite = {}\n", m["abc"]);
    // test copy
    var m2: Map<string, int> = m;
    printf("m2[\"abc\"] = {}\n", m2["abc"]);
    printf("m2[\"d\"] = {}\n", m2["d"]);
    // test iteration
    println("iterating m2:");
    for v in m2 {
        printf("  v={}\n", *v);
    }
    println("");
}

func test_shared() {
    println("testing shared:");
    var r1 = Shared<int>.from_value(42);
    printf("r1.as_ref()={}, ref_count={}\n", r1.as_ref(), r1.ref_count());
    var r2: Shared<int> = r1;
    printf("after copy: ref_count={}\n", r1.ref_count());
    printf("r2.as_ref()={}\n", r2.as_ref());
}

struct NestedState {
    value: int = 0;
    count: int = 0;
}

struct NestedShared {
    data: Shared<NestedState>;

    mut func new() {
        this.data = Shared<NestedState>.from_value({});
    }

    func get_count() int {
        return this.data.as_ref().count;
    }

    func ref_count() uint32 {
        return this.data.ref_count();
    }
}

func test_nested_shared() {
    println("testing nested shared:");
    var ns = NestedShared{};
    printf("ns.ref_count()={}\n", ns.ref_count());
    printf("ns.get_count()={}\n", ns.get_count());
    println("");
}

func test_string() {
    println("testing string:");
    var s1 = "Hello";
    printf("s1.length={}\n", s1.length);
    printf("s1.is_empty()={}\n", s1.is_empty());
    var s2 = s1 + " World";
    printf("s1 + \" World\"={}\n", s2);
    var empty = "";
    printf("empty.is_empty()={}\n", empty.is_empty());
    println("");
}

struct Traced {
    id: int = 0;

    mut func new(id: int) {
        this.id = id;
        printf("  Traced.new({})\n", id);
    }

    mut func delete() {
        printf("  Traced.delete({})\n", this.id);
    }

    impl ops.Copy {
        mut func copy(source: &This) {
            this.id = source.id;
            printf("  Traced.copy({})\n", source.id);
        }
    }
}

struct TracedValue {
    id: int = 0;

    mut func delete() {
        if this.id != 0 {
            printf("  TracedValue.delete({})\n", this.id);
        }
    }

    impl ops.Copy {
        mut func copy(source: &This) {
            this.id = source.id;
        }
    }
}

func test_map_int_key() {
    println("testing map int key:");
    var m = Map<int, int>{};
    m.set(1, 100);
    m.set(2, 200);
    m.set(3, 300);
    printf("m[1] = {}\n", m[1]);
    printf("m[2] = {}\n", m[2]);
    printf("m[3] = {}\n", m[3]);
    // overwrite
    m.set(2, 999);
    printf("m[2] after overwrite = {}\n", m[2]);
    // get optional
    printf("m.get(1) = {}\n", m.get(1)!);
    printf("m.get(99) = {}\n", m.get(99));
    // remove
    m.remove(2);
    printf("m.get(2) after remove = {}\n", m.get(2));
    printf("m[1] still = {}\n", m[1]);
    println("");
}

struct MapTestPoint {
    x: int;
    y: int;

    mut func new(x: int, y: int) {
        this.x = x;
        this.y = y;
    }

    impl ops.Hash {
        func hash() uint64 {
            var h = (this.x as uint64) * 31;
            h = h + (this.y as uint64);
            return h;
        }
    }

    impl ops.Eq {
        func eq(other: &This) bool {
            return this.x == other.x && this.y == other.y;
        }
    }
}

func test_map_custom_key() {
    println("testing map custom key:");
    var m = Map<MapTestPoint, string>{};
    m.set(MapTestPoint{1, 2}, "alpha");
    m.set(MapTestPoint{3, 4}, "beta");
    m.set(MapTestPoint{5, 6}, "gamma");
    printf("m[(1,2)] = {}\n", m[MapTestPoint{1, 2}]);
    printf("m[(3,4)] = {}\n", m[MapTestPoint{3, 4}]);
    printf("m[(5,6)] = {}\n", m[MapTestPoint{5, 6}]);
    // overwrite
    m.set(MapTestPoint{1, 2}, "replaced");
    printf("m[(1,2)] after overwrite = {}\n", m[MapTestPoint{1, 2}]);
    // remove
    m.remove(MapTestPoint{3, 4});
    printf("m.get((3,4)) after remove = {}\n", m.get(MapTestPoint{3, 4}));
    println("");
}

func test_map_lifecycle_helper() Map<string, TracedValue> {
    var m = Map<string, TracedValue>{};
    m.set("a", TracedValue{id: 10});
    m.set("b", TracedValue{id: 20});
    printf("m[\"a\"].id={}\n", m["a"].id);
    printf("m[\"b\"].id={}\n", m["b"].id);
    return m;
}

func test_map_lifecycle() {
    println("testing map lifecycle:");
    var m = test_map_lifecycle_helper();
    printf("after return: m[\"a\"].id={}\n", m["a"].id);
    println("before scope exit:");
}

func test_box_helper() {
    println("creating box:");
    var t = new Traced{1};
    var b1 = Box<Traced>{t};
    printf("b1.id={}\n", b1.as_ref().id);
    println("copying box:");
    var b2 = b1;
    printf("b2.id={}\n", b2.as_ref().id);
    println("before scope exit:");
}

func test_box() {
    println("testing box:");
    var b1 = Box.from_value(42);
    printf("b1.as_ref()={}\n", b1.as_ref());
    var b2 = b1;
    printf("after copy: b1={}, b2={}\n", b1.as_ref(), b2.as_ref());
    test_box_helper();
    println("after helper returned");
    println("");
}

struct DisplayInner {
    value: int = 5;
    private hidden: int = 99;
}

struct DisplayOuter {
    name: string = "chi";
    protected inner: DisplayInner = {};
    private secret: int = 42;
}

struct TracedStringField {
    value: string = "";

    mut func delete() {
        if !this.value.is_empty() {
            printf("  TracedStringField.delete({})\n", this.value);
        }
    }

    impl ops.Copy {
        mut func copy(source: &This) {
            this.value = source.value;
            printf("  TracedStringField.copy({})\n", source.value);
        }
    }
}

struct NamedStringFields {
    first: TracedStringField = {};
    second: TracedStringField = {};
}

func make_named_string_fields(first: TracedStringField, second: TracedStringField) NamedStringFields {
    return {:first, :second};
}

func test_struct_display() {
    println("testing struct display:");
    println(DisplayInner{});
    println(DisplayOuter{});
}

func test_named_field_construct() {
    println("testing named field construct:");
    var first = TracedStringField{value: "left"};
    var second = TracedStringField{value: "right"};
    let fields = make_named_string_fields(first, second);
    printf("fields.first={}\n", fields.first.value);
    printf("fields.second={}\n", fields.second.value);
    println("before scope exit:");
    println("");
}

func main() {
    test_optional();
    test_array();
    test_map();
    test_map_int_key();
    test_map_custom_key();
    test_map_lifecycle();
    test_shared();
    test_nested_shared();
    test_string();
    test_box();
    test_named_field_construct();
    test_struct_display();
}
