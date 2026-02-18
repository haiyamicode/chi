// Test safe mode lifetime analysis (-s flag)

// --- Basic struct with reference field ---

struct Holder {
    ref: &int = null;

    mut func store(r: &'This int) {
        this.ref = r;
    }

    func get() &int {
        return this.ref;
    }
}

// --- Struct with multiple reference fields ---

struct MultiRef {
    a: &int = null;
    b: &int = null;

    mut func set_both(x: &'This int, y: &'This int) {
        this.a = x;
        this.b = y;
    }

    mut func set_a(x: &'This int) {
        this.a = x;
    }
}

// --- Return struct by value with param refs ---

func make_holder(x: &int) Holder {
    var h = Holder{};
    // 'This annotation on store's param creates edge h -> x at caller site.
    // Returning h by value: param x outlives the function, so this is fine.
    h.store(x);
    return h;
}

func make_multi(x: &int, y: &int) MultiRef {
    var m = MultiRef{};
    m.set_both(x, y);
    return m;
}

// --- Passing references locally (no escape) ---

func use_ref(r: &int) int {
    return r! + 1;
}

func local_ref_use() int {
    var x = 10;
    return use_ref(&x);
}

// --- Chained struct operations ---

func chain_test(val: &int) int {
    var h = Holder{};
    h.store(val);
    return h.get()!;
}

// --- Nested struct with references ---

struct Pair {
    first: &int = null;
    second: &int = null;

    mut func set_first(v: &'This int) {
        this.first = v;
    }

    mut func set_second(v: &'This int) {
        this.second = v;
    }

    func sum() int {
        return this.first! + this.second!;
    }
}

func make_pair(a: &int, b: &int) Pair {
    var p = Pair{};
    p.set_first(a);
    p.set_second(b);
    return p;
}

// --- Reference to param (not local) ---

func ref_to_param(x: &int) &int {
    return x;
}

// --- Struct returned by value, fields set via method ---

func test_holder() {
    printf("=== holder ===\n");
    var val = 42;
    var h = make_holder(&val);
    printf("h.ref = {}\n", h.get()!);
}

func test_multi_ref() {
    printf("=== multi ref ===\n");
    var a = 10;
    var b = 20;
    var m = make_multi(&a, &b);
    printf("a = {}, b = {}\n", m.a!, m.b!);
}

func test_local_ref() {
    printf("=== local ref ===\n");
    printf("result = {}\n", local_ref_use());
}

func test_chain() {
    printf("=== chain ===\n");
    var v = 99;
    printf("chain = {}\n", chain_test(&v));
}

func test_pair() {
    printf("=== pair ===\n");
    var x = 3;
    var y = 7;
    var p = make_pair(&x, &y);
    printf("sum = {}\n", p.sum());
}

func test_ref_to_param() {
    printf("=== ref to param ===\n");
    var val = 55;
    var r = ref_to_param(&val);
    printf("ref = {}\n", r!);
}

// --- Multiple stores to same field ---

func test_reassign() {
    printf("=== reassign ===\n");
    var a = 1;
    var b = 2;
    var h = Holder{};
    h.store(&a);
    printf("first = {}\n", h.get()!);
    h.store(&b);
    printf("second = {}\n", h.get()!);
}

// --- Struct with ref field, no method, direct init ---

func test_direct_init() {
    printf("=== direct init ===\n");
    var val = 77;
    var h = Holder{ref: &val};
    printf("direct = {}\n", h.ref!);
}

func main() {
    test_holder();
    test_multi_ref();
    test_local_ref();
    test_chain();
    test_pair();
    test_ref_to_param();
    test_reassign();
    test_direct_init();
}
