// Test safe mode lifetime analysis (-s flag)

import "std/ops" as ops;

// --- Basic struct with reference field ---

struct Holder {
    ref: &int;

    mut func new(r: &'this int) {
        this.ref = r;
    }

    mut func store(r: &'this int) {
        this.ref = r;
    }

    func get() &int {
        return this.ref;
    }
}

// --- Struct with multiple reference fields ---

struct MultiRef {
    a: &int;
    b: &int;

    mut func new(a: &'this int, b: &'this int) {
        this.a = a;
        this.b = b;
    }

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
    return {x};
}

func make_multi(x: &int, y: &int) MultiRef {
    return {x, y};
}

// --- Passing references locally (no escape) ---

func use_ref(r: &int) int {
    return *r + 1;
}

func local_ref_use() int {
    var x = 10;
    return use_ref(&x);
}

// --- Chained struct operations ---

func chain_test(val: &int) int {
    var h = Holder{val};
    return *h.get();
}

// --- Nested struct with references ---

struct Pair {
    first: &int;
    second: &int;

    mut func new(first: &'this int, second: &'this int) {
        this.first = first;
        this.second = second;
    }

    mut func set_first(v: &'this int) {
        this.first = v;
    }

    mut func set_second(v: &'this int) {
        this.second = v;
    }

    func sum() int {
        return *this.first + *this.second;
    }
}

func make_pair(a: &int, b: &int) Pair {
    return {a, b};
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
    printf("h.ref = {}\n", *h.get());
}

func test_multi_ref() {
    printf("=== multi ref ===\n");
    var a = 10;
    var b = 20;
    var m = make_multi(&a, &b);
    printf("a = {}, b = {}\n", *m.a, *m.b);
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
    printf("ref = {}\n", *r);
}

// --- Multiple stores to same field ---

func test_reassign() {
    printf("=== reassign ===\n");
    var a = 1;
    var b = 2;
    var h = Holder{&a};
    printf("first = {}\n", *h.get());
    h.store(&b);
    printf("second = {}\n", *h.get());
}

// --- Struct with ref field, no method, direct init ---

func test_direct_init() {
    printf("=== direct init ===\n");
    var val = 77;
    var h = Holder{&val};
    printf("direct = {}\n", *h.ref);
}

// --- Intra-function: local declared before holder (correct order) ---

func test_local_order() {
    printf("=== local order ===\n");
    var local = 88;
    var h = Holder{&local};
    printf("order = {}\n", *h.get());
}

// --- Move semantics: &move (ownership transfer) ---

struct Resource {
    name: string;

    mut func new(n: string) {
        this.name = n;
    }

    mut func delete() {
        printf("Resource.delete({})\n", this.name);
    }

    impl ops.Copy {
        mut func copy(source: &This) {
            this.name = source.name;
        }
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
    unsafe {
        delete a;
    }
    // a is sunk — RAII does not destroy again
}

// --- Move semantics: move (value optimization) ---

struct Heavy {
    value: int;

    mut func delete() {
        printf("Heavy.delete({})\n", this.value);
    }

    impl ops.Copy {
        mut func copy(source: &This) {
            printf("Heavy.copy({})\n", source.value);
            this.value = source.value;
        }
    }
}

func consume_heavy(h: Heavy) {
    printf("consumed: {}\n", h.value);
}

// move x skips copy, sinks source
func test_value_move() {
    printf("=== value move ===\n");
    var a = Heavy{value: 42};
    var b = move a;
    printf("b.value = {}\n", b.value);
}

// Regular copy invokes copy
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
        *p = 42;
        printf("p = {}\n", *p);
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
    val: &int;

    mut func new(v: &'this int) {
        this.val = v;
    }
}

func get_ref(h: &RefHolder) &int {
    return h.val;
}

// Multiple ref params returning either — shared lifetime allows both
func bigger_ref<'a>(a: &'a int, b: &'a int) &'a int {
    if *a > *b {
        return a;
    }
    return b;
}

func test_cross_fn_ref() {
    printf("=== cross fn ref ===\n");
    var x = 99;
    var h = RefHolder{&x};
    var r = get_ref(&h);
    printf("get_ref = {}\n", *r);
}

func test_bigger_ref() {
    printf("=== bigger ref ===\n");
    var a = 10;
    var b = 20;
    var big = bigger_ref(&a, &b);
    printf("bigger = {}\n", *big);
}

// Method returning reference (elision to 'this)
func test_method_ref_return() {
    printf("=== method ref ===\n");
    var val = 42;
    var h = Holder{&val};
    var r = h.get();
    printf("method ref = {}\n", *r);
}

struct PointRef {
    x: int;
}

func alias_point_field(p: &PointRef) &int {
    var r = &p.x;
    return r;
}

func alias_destructured_field(p: &PointRef) &int {
    var {&x} = *p;
    return x;
}

func test_ref_alias_return() {
    printf("=== ref alias return ===\n");
    var p = PointRef{x: 77};
    printf("field alias = {}\n", *alias_point_field(&p));
    printf("destructure alias = {}\n", *alias_destructured_field(&p));
}

// --- Block-based destruction ---

// Borrow scoped in block, delete after block is safe
func test_block_scoped_borrow() {
    printf("=== block scoped borrow ===\n");
    var x = new Resource{"block_res"};
    {
        var r: &Resource = x;
        printf("r.name = {}\n", r.name);
    }
    // r is out of scope, safe to delete
    unsafe {
        delete x;
    }
}

// Block destruction order: inner vars destroyed before outer
func test_block_destruction_order() {
    printf("=== block destruction order ===\n");
    var outer = Heavy{value: 1};
    {
        var inner1 = Heavy{value: 2};
        var inner2 = Heavy{value: 3};
        printf("inner: {} {}\n", inner1.value, inner2.value);
    }
    printf("outer still alive: {}\n", outer.value);
}

// Early return from nested block: all blocks cleaned up
func test_early_return_cleanup() Heavy {
    var a = Heavy{value: 10};
    {
        var b = Heavy{value: 20};
        return move b;
    }
}

// Break from loop with block-local vars: cleaned up before exiting
func test_break_cleanup() {
    printf("=== break cleanup ===\n");
    for i in 0..3 {
        var h = Heavy{value: 100 + i};
        if i == 1 {
            printf("breaking at {}\n", h.value);
            break;
        }
        printf("iter {}\n", h.value);
    }
    printf("after loop\n");
}

// Continue from loop: block vars cleaned up each iteration
func test_continue_cleanup() {
    printf("=== continue cleanup ===\n");
    for i in 0..3 {
        var h = Heavy{value: 200 + i};
        if i == 1 {
            printf("skipping {}\n", h.value);
            continue;
        }
        printf("iter {}\n", h.value);
    }
    printf("after loop\n");
}

// --- Move tests ---

// Value move into function arg — source sunk, not double-destroyed
func test_move_to_fn_arg() {
    printf("=== move to fn arg ===\n");
    var h = Heavy{value: 55};
    consume_heavy(move h);
    // h is sunk — not destroyed here
}

// Move in inner block — moved var skipped by block cleanup
func test_move_block_cleanup() {
    printf("=== move block cleanup ===\n");
    var a = Heavy{value: 60};
    {
        var b = move a;
        printf("b = {}\n", b.value);
        // b destroyed at block exit
    }
    printf("after block\n");
    // a was moved — not destroyed at function exit
}

// Move in loop — each iteration creates and moves, no double-destroy
func test_move_in_loop() {
    printf("=== move in loop ===\n");
    for i in 0..3 {
        var h = Heavy{value: 300 + i};
        consume_heavy(move h);
        // h sunk each iteration
    }
}

// &move pointer move in inner block — only destination destroyed
func test_move_ptr_block() {
    printf("=== move ptr block ===\n");
    var a = new Resource{"rho"};
    {
        var b = a; // &move: ownership transfer
        printf("b.name = {}\n", b.name);
        // b destroyed at block exit
    }
    printf("after block\n");
    // a was moved — not destroyed
}

// --- Branch-aware lifetime analysis ---

// Maybe-move: moved in one branch, drop flag handles cleanup
func test_branch_maybe_move() {
    printf("=== branch maybe move ===\n");
    var h = Heavy{value: 100};
    if true {
        consume_heavy(move h);
    }
    printf("after if (true)\n");

    var h2 = Heavy{value: 101};
    if false {
        consume_heavy(move h2);
    }
    printf("after if (false)\n");
}

// Guard clause: move + return in then branch, no drop flag needed
func test_branch_guard_clause() {
    printf("=== branch guard clause ===\n");
    var h = Heavy{value: 200};
    if false {
        consume_heavy(move h);
        return;
    }
    printf("h alive: {}\n", h.value);
}

// Both branches move: definite sink, no cleanup at function end
func test_branch_both_move() {
    printf("=== branch both move ===\n");
    var h = Heavy{value: 300};
    if true {
        consume_heavy(move h);
    } else {
        consume_heavy(move h);
    }
    printf("after if\n");
}

// Nested branches: maybe-move through nested ifs
func test_branch_nested_maybe_move() {
    printf("=== branch nested maybe move ===\n");
    var h = Heavy{value: 400};
    if true {
        if true {
            consume_heavy(move h);
        }
    }
    printf("after nested if\n");
}

// Nested: move in inner if, outer else — different paths
func test_branch_nested_mixed() {
    printf("=== branch nested mixed ===\n");
    var h = Heavy{value: 500};
    if true {
        consume_heavy(move h);
    } else {
        if true {
            consume_heavy(move h);
        } else {
            consume_heavy(move h);
        }
    }
    printf("after nested mixed\n");
}

// Guard clause with else: move in both, both terminate
func test_branch_both_terminate() {
    printf("=== branch both terminate ===\n");
    var h = Heavy{value: 600};
    var result = 0;
    if true {
        consume_heavy(move h);
        result = 1;
    } else {
        consume_heavy(move h);
        result = 2;
    }
    printf("result: {}\n", result);
}

// Else branch sees variable as alive (not poisoned by then's move)
func test_branch_else_alive() {
    printf("=== branch else alive ===\n");
    var h = Heavy{value: 700};
    if false {
        consume_heavy(move h);
    } else {
        printf("h alive in else: {}\n", h.value);
    }
}

// Switch: move in one case only (maybe-move, needs drop flag)
func test_switch_maybe_move() {
    printf("=== switch maybe move ===\n");
    var h = Heavy{value: 800};
    var x = 1;
    switch x {
        1 => consume_heavy(move h),
        else => {}
    }
    printf("after switch\n");
}

// Switch: move in all cases of exhaustive switch (definite sink)
func test_switch_all_move() {
    printf("=== switch all move ===\n");
    var h = Heavy{value: 900};
    var x = 1;
    switch x {
        1 => consume_heavy(move h),
        else => consume_heavy(move h)
    }
    printf("after switch\n");
}

// Switch: guard clause — one case moves and returns
func test_switch_guard_clause() {
    printf("=== switch guard clause ===\n");
    var h = Heavy{value: 1000};
    var x = 2;
    switch x {
        1 => {
            consume_heavy(move h);
            return;
        },
        else => {}
    }
    printf("h alive: {}\n", h.value);
}

// Switch: else branch sees variable alive
func test_switch_else_alive() {
    printf("=== switch else alive ===\n");
    var h = Heavy{value: 1100};
    var x = 99;
    switch x {
        1 => consume_heavy(move h),
        else => printf("h alive in else: {}\n", h.value)
    }
}

// --- Try/catch branch-aware tests ---

struct TestError {
    impl Error {
        func message() string {
            return "test error";
        }
    }
}

func may_throw(should_throw: bool) {
    if should_throw {
        throw new TestError{};
    }
}

// Try/catch: move in catch only (maybe-move, needs drop flag)
func test_try_catch_maybe_move() {
    printf("=== try catch maybe move ===\n");
    var h = Heavy{value: 1200};
    try may_throw(true) catch {
        consume_heavy(move h);
    };
    printf("after try\n");
}

// Try/catch: catch with return (guard clause pattern)
func test_try_catch_guard() {
    printf("=== try catch guard ===\n");
    var h = Heavy{value: 1300};
    try may_throw(false) catch {
        consume_heavy(move h);
        return;
    };
    printf("h alive: {}\n", h.value);
}

func get_val_lt<'a, T: 'a>(val: T) T {
    return val;
}

func test_generic_lifetime_bound() {
    printf("=== generic lifetime bound ===\n");
    var x = 42;
    var r = get_val_lt<&int>(&x);
    printf("r = {}\n", *r);
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
    test_ref_alias_return();
    test_block_scoped_borrow();
    test_block_destruction_order();
    var ret = test_early_return_cleanup();
    printf("=== early return cleanup ===\n");
    printf("returned: {}\n", ret.value);
    test_break_cleanup();
    test_continue_cleanup();
    test_move_to_fn_arg();
    test_move_block_cleanup();
    test_move_in_loop();
    test_move_ptr_block();
    test_generic_lifetime_bound();
    test_branch_maybe_move();
    test_branch_guard_clause();
    test_branch_both_move();
    test_branch_nested_maybe_move();
    test_branch_nested_mixed();
    test_branch_both_terminate();
    test_branch_else_alive();
    test_switch_maybe_move();
    test_switch_all_move();
    test_switch_guard_clause();
    test_switch_else_alive();
    test_try_catch_maybe_move();
    test_try_catch_guard();
}
