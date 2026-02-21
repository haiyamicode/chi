// Test safe mode lifetime analysis (-s flag)

// --- Basic struct with reference field ---

struct Holder {
    ref: &int = null;

    mut func store(r: &'this int) {
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

    mut func set_both(x: &'this int, y: &'this int) {
        this.a = x;
        this.b = y;
    }

    mut func set_a(x: &'this int) {
        this.a = x;
    }
}

// --- Return struct by value with param refs ---

func make_holder(x: &int) Holder {
    var h = Holder{};
    // 'this annotation on store's param creates edge h -> x at caller site.
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

    mut func set_first(v: &'this int) {
        this.first = v;
    }

    mut func set_second(v: &'this int) {
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

// --- Intra-function: local declared before holder (correct order) ---

func test_local_order() {
    printf("=== local order ===\n");
    var local = 88;
    var h = Holder{};
    h.store(&local);
    printf("order = {}\n", h.get()!);
}

// --- Move semantics: &move (ownership transfer) ---

struct Resource {
    name: string;

    func new(n: string) {
        this.name = n;
    }

    func delete() {
        printf("Resource.delete({})\n", this.name);
    }
}

func take_ownership(r: &move Resource) {
    printf("took: {}\n", r.name);
    // r is auto-destroyed at function return (RAII)
}

// &move T passed to &move param — ownership transfer, RAII in callee
func test_move_to_fn() {
    printf("=== move to fn ===\n");
    var r = new Resource{"alpha"};
    take_ownership(r);
}

// &move T assigned to new var — ownership transfer, RAII on new owner
func test_move_to_var() {
    printf("=== move to var ===\n");
    var a = new Resource{"beta"};
    var b = a;
    printf("b.name = {}\n", b.name);
    // b is auto-destroyed at scope exit (RAII)
}

// &move T borrowed as & — no move, source still valid, RAII on source
func test_borrow_from_move() {
    printf("=== borrow from move ===\n");
    var a = new Resource{"gamma"};
    var r: &Resource = a;
    printf("r.name = {}\n", r.name);
    printf("a.name = {}\n", a.name);
    // a is auto-destroyed at scope exit (RAII)
}

// RAII: no explicit delete needed, auto-destroyed at scope exit
func test_raii() {
    printf("=== raii ===\n");
    var a = new Resource{"delta"};
    printf("a.name = {}\n", a.name);
}

// Explicit delete sinks the variable, RAII skips it
func test_early_delete() {
    printf("=== early delete ===\n");
    var a = new Resource{"epsilon"};
    delete a;
    // a is sunk — RAII does not destroy again
}

// --- Move semantics: move (value optimization) ---

import "std/ops" as ops;

struct Heavy {
    value: int;

    func delete() {
        printf("Heavy.delete({})\n", this.value);
    }

    impl ops.CopyFrom<Heavy> {
        func copy_from(source: &Heavy) {
            printf("Heavy.copy({})\n", source.value);
            this.value = source.value;
        }
    }
}

// move x skips copy_from, sinks source
func test_value_move() {
    printf("=== value move ===\n");
    var a = Heavy{value: 42};
    var b = move a;
    printf("b.value = {}\n", b.value);
}

// Regular copy invokes copy_from
func test_value_copy() {
    printf("=== value copy ===\n");
    var a = Heavy{value: 99};
    var b = a;
    printf("a={}, b={}\n", a.value, b.value);
}

// --- Unsafe blocks ---

import "std/mem" as mem;

unsafe func unsafe_add(a: int, b: int) int {
    return a + b;
}

// unsafe block allows calling unsafe functions in safe mode
func test_unsafe_block() {
    printf("=== unsafe block ===\n");
    unsafe {
        var result = unsafe_add(10, 20);
        printf("result = {}\n", result);

        var p = mem.malloc(sizeof int) as *int;
        p! = 42;
        printf("p = {}\n", p!);
        mem.free(p as *void);
    }
}

// unsafe function can call other unsafe functions without a block
unsafe func unsafe_caller() int {
    return unsafe_add(3, 4);
}

func test_unsafe_fn_calls_unsafe() {
    printf("=== unsafe fn calls unsafe ===\n");
    unsafe {
        printf("result = {}\n", unsafe_caller());
    }
}

// --- Cross-function reference return (elision) ---

struct RefHolder {
    val: &int = null;
}

func get_ref(h: &RefHolder) &int {
    return h.val;
}

// Multiple ref params returning either — shared lifetime allows both
func bigger_ref<'a>(a: &'a int, b: &'a int) &'a int {
    if a! > b! {
        return a;
    }
    return b;
}

func test_cross_fn_ref() {
    printf("=== cross fn ref ===\n");
    var x = 99;
    var h = RefHolder{val: &x};
    var r = get_ref(&h);
    printf("get_ref = {}\n", r!);
}

func test_bigger_ref() {
    printf("=== bigger ref ===\n");
    var a = 10;
    var b = 20;
    var big = bigger_ref(&a, &b);
    printf("bigger = {}\n", big!);
}

// Method returning reference (elision to 'this)
func test_method_ref_return() {
    printf("=== method ref ===\n");
    var val = 42;
    var h = Holder{};
    h.store(&val);
    var r = h.get();
    printf("method ref = {}\n", r!);
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
    test_local_order();
    test_move_to_fn();
    test_move_to_var();
    test_borrow_from_move();
    test_raii();
    test_early_delete();
    test_value_move();
    test_value_copy();
    test_unsafe_block();
    test_unsafe_fn_calls_unsafe();
    test_cross_fn_ref();
    test_bigger_ref();
    test_method_ref_return();
}

