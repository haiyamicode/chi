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
    println("");
}

func test_array() {
    println("testing array:");
    var a: Array<int> = {};
    a.add(1);
    a.add(2);
    printf("a=[{}, {}]\n", a[0], a[1]);
    a[0] = 2;
    a[1] = 1;
    printf("a=[{}, {}]\n", a[0], a[1]);
    var b = Array<int>{10, 20, 30};
    printf("b=[{}, {}, {}]\n", b[0], b[1], b[2]);
    println("");
}

func test_map() {
    println("testing map:");
    var m: Map<string, int> = {};
    m["abc"] = 1;
    m["d"] = 2;
    printf("m[\"abc\"] = {}\n", m["abc"]);
    printf("m[\"d\"] = {}\n", m["d"]);
    printf("m[\"ef\"] = {}\n", m["ef"]);
    var it1 = m.find("abc");
    printf("m.find(\"abc\")!! = {}\n", it1!!);
    var it2 = m.find("invalid");
    printf("m.find(\"invalid\") = {}\n", it2);
    println("");
}

func test_shared() {
    println("testing shared:");
    var r1: Shared<int> = {42};
    printf("r1.as_ref()={}, ref_count={}\n", r1.as_ref()!, r1.ref_count());
    var r2: Shared<int> = r1;
    printf("after copy: ref_count={}\n", r1.ref_count());
    r1.set(100);
    printf("after r1.set(100): r2.as_ref()={}\n", r2.as_ref()!);
}

struct NestedState {
    value: int = 0;
    count: int = 0;
}

struct NestedShared {
    data: Shared<NestedState>;

    func new() {
        this.data = {{}};
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
    var ns: NestedShared = {};
    printf("ns.ref_count()={}\n", ns.ref_count());
    printf("ns.get_count()={}\n", ns.get_count());
    println("");
}

func test_string() {
    println("testing string:");
    var s1 = "Hello";
    printf("s1.length={}\n", s1.length);
    printf("s1.is_empty()={}\n", s1.is_empty());
    var s2 = s1.add(" World");
    printf("s1.add(\" World\")={}\n", s2);
    var empty = "";
    printf("empty.is_empty()={}\n", empty.is_empty());
    println("");
}

func test_box() {
    println("testing box:");
    var b1 = Box<int>{42};
    printf("b1.as_ref()={}\n", b1.as_ref()!);
    b1.set(99);
    printf("after set: b1.as_ref()={}\n", b1.as_ref()!);
    var b2 = b1;
    b2.set(7);
    printf("after copy+set: b1={}, b2={}\n", b1.as_ref()!, b2.as_ref()!);
    println("");
}

func main() {
    test_optional();
    test_array();
    test_map();
    test_shared();
    test_nested_shared();
    test_string();
    test_box();
}

