import "std/ops" as ops;

struct Inner1 {
    id: int = 0;

    mut func new(id: int) {
        this.id = id;
        printf("  Inner1.new({})\n", id);
    }

    mut func delete() {
        printf("  Inner1.delete({})\n", this.id);
    }

    impl ops.Copy {
        mut func copy(source: &This) {
            this.id = source.id;
        }
    }
}

struct Outer1 {
    inner: Inner1;

    mut func new(id: int) {
        println("Outer1.new");
        this.inner = {id};
    }
}

func test_auto_destroy_helper() {
    var o = Outer1{1};
    println("Before function exit");
}

func test_auto_destroy_no_custom_delete() {
    println("=== Test 1: Auto-destroy without custom delete ===");
    test_auto_destroy_helper();
    println("After helper returned");
    println("");
}

struct WithDefaults {
    a: int = 10;
    b: int = 20;
    c: int;

    mut func new(c_val: int) {
        printf("WithDefaults.new: a={}, b={}, setting c={}\n", this.a, this.b, c_val);
        this.c = c_val;
    }
}

func test_new_initializes_defaults() {
    println("=== Test 2: __new initializes defaults before user's new() ===");
    var w = WithDefaults{30};
    printf("Final: a={}, b={}, c={}\n", w.a, w.b, w.c);
    println("");
}

struct OrderedField {
    name: string;

    mut func new(name: string) {
        this.name = name;
        printf("  OrderedField.new('{}')\n", name);
    }

    mut func delete() {
        printf("  OrderedField.delete('{}')\n", this.name);
    }

    impl ops.Copy {
        mut func copy(source: &This) {
            this.name = source.name;
        }
    }
}

struct OrderedContainer {
    first: OrderedField;
    second: OrderedField;
    third: OrderedField;

    mut func new() {
        println("OrderedContainer.new");
        this.first = {"first"};
        this.second = {"second"};
        this.third = {"third"};
    }

    mut func delete() {
        println("OrderedContainer.delete (user)");
    }

    impl ops.Copy {
        mut func copy(source: &This) {
            this.first = source.first;
            this.second = source.second;
            this.third = source.third;
        }
    }
}

func test_destruction_order_helper() {
    var o = OrderedContainer{};
    println("Before function exit");
}

func test_destruction_order() {
    println("=== Test 3: Destruction order (reverse of declaration) ===");
    test_destruction_order_helper();
    println("After helper returned");
    println("");
}

struct RefCountedData {
    value: int = 0;

    mut func new(v: int) {
        this.value = v;
        printf("  RefCountedData.new({})\n", v);
    }

    mut func delete() {
        printf("  RefCountedData.delete({})\n", this.value);
    }

    impl ops.Copy {
        mut func copy(source: &This) {
            this.value = source.value;
        }
    }
}

struct HoldsShared {
    data: Shared<RefCountedData>;

    mut func new(v: int) {
        println("HoldsShared.new");
        this.data = {new {v}};
        printf("  ref_count after construction: {}\n", this.data.ref_count());
    }
}

func test_shared_helper() {
    var h = HoldsShared{42};
    println("Before function exit");
}

func test_shared_auto_destroy() {
    println("=== Test 4: Shared<T> field auto-destruction ===");
    test_shared_helper();
    println("After helper - RefCountedData should be deleted");
    println("");
}

struct Level3 {
    id: int = 0;

    mut func delete() {
        printf("    Level3.delete({})\n", this.id);
    }

    impl ops.Copy {
        mut func copy(source: &This) {
            this.id = source.id;
        }
    }
}

struct Level2 {
    id: int = 0;
    child: Level3;

    mut func new(id: int) {
        this.id = id;
        this.child = {id: id * 10};
    }

    mut func delete() {
        printf("  Level2.delete({})\n", this.id);
    }

    impl ops.Copy {
        mut func copy(source: &This) {
            this.id = source.id;
            this.child = source.child;
        }
    }
}

struct Level1 {
    id: int = 0;
    child: Level2;

    mut func new(id: int) {
        this.id = id;
        this.child = {id * 10};
    }

    mut func delete() {
        printf("Level1.delete({})\n", this.id);
    }

    impl ops.Copy {
        mut func copy(source: &This) {
            this.id = source.id;
            this.child = source.child;
        }
    }
}

func test_nested_helper() {
    var l = Level1{1};
    println("Before function exit");
}

func test_nested_destruction() {
    println("=== Test 5: Nested auto-destruction (3 levels) ===");
    test_nested_helper();
    println("After helper returned");
    println("");
}

struct OptionalData {
    value: int = 0;

    mut func new(v: int) {
        this.value = v;
        printf("  OptionalData.new({})\n", v);
    }

    mut func delete() {
        printf("  OptionalData.delete({})\n", this.value);
    }

    impl ops.Copy {
        mut func copy(source: &This) {
            this.value = source.value;
        }
    }
}

struct HoldsOptionalShared {
    data: ?Shared<OptionalData> = null;

    mut func new(v: int) {
        println("HoldsOptionalShared.new");
        this.data! = {new {v}};
        printf("  ref_count: {}\n", this.data!.ref_count());
    }
}

struct HoldsNullOptional {
    data: ?Shared<OptionalData> = null;
}

func test_optional_with_value_helper() {
    var h = HoldsOptionalShared{99};
    println("Before function exit (with value)");
}

func test_optional_null_helper() {
    var h = HoldsNullOptional{};
    println("Before function exit (null)");
}

func test_optional_auto_destroy() {
    println("=== Test 6: Optional<Shared<T>> field auto-destruction ===");
    test_optional_with_value_helper();
    println("After helper - OptionalData should be deleted");
    test_optional_null_helper();
    println("After null helper - no crash expected");
    println("");
}

struct DirectStruct {
    id: int = 0;

    mut func new(id: int) {
        this.id = id;
        printf("  DirectStruct.new({})\n", id);
    }

    mut func delete() {
        printf("  DirectStruct.delete({})\n", this.id);
    }

    impl ops.Copy {
        mut func copy(source: &This) {
            this.id = source.id;
        }
    }
}

struct HoldsOptionalDirect {
    data: ?DirectStruct = null;

    mut func new(id: int) {
        println("HoldsOptionalDirect.new");
        this.data! = {id};
    }
}

func test_optional_direct_helper() {
    var h = HoldsOptionalDirect{77};
    println("Before function exit");
}

func test_optional_direct() {
    println("=== Test 7: Optional<T> with direct struct ===");
    test_optional_direct_helper();
    println("After helper - DirectStruct should be deleted");
    println("");
}

struct BothLifecycles {
    default_val: int = 999;
    inner: Inner1;

    mut func new(id: int) {
        printf("BothLifecycles.new: default_val={}\n", this.default_val);
        this.inner = {id};
    }
}

func test_both_helper() {
    var b = BothLifecycles{88};
    println("Before function exit");
}

func test_both_lifecycles() {
    println("=== Test 8: Both __new and __delete on same struct ===");
    test_both_helper();
    println("After helper returned");
    println("");
}

struct TrackedVar {
    name: string;

    mut func new(name: string) {
        this.name = name;
        printf("  TrackedVar.new('{}')\n", name);
    }

    mut func delete() {
        printf("  TrackedVar.delete('{}')\n", this.name);
    }

    impl ops.Copy {
        mut func copy(source: &This) {
            this.name = source.name;
            printf("  TrackedVar.copy('{}')\n", source.name);
        }
    }
}

func test_multiple_vars_helper() {
    var a = TrackedVar{"first"};
    var b = TrackedVar{"second"};
    var c = TrackedVar{"third"};
    println("Before function exit - destroys in reverse declaration order");
}

func test_multiple_vars() {
    println("=== Test 9: Multiple local variables destruction order ===");
    test_multiple_vars_helper();
    println("After helper returned");
    println("");
}

func make_tracked(name: string) TrackedVar {
    return {name};
}

func consume_tracked(t: TrackedVar) {}

func identity_tracked(t: TrackedVar) TrackedVar {
    return t;
}

func test_temp_cleanup() {
    println("=== Test 10: Temporary cleanup (no leaks) ===");

    println("--- fn call arg ---");
    consume_tracked(make_tracked("fn-arg"));
    println("after consume(make())");

    println("--- nested fn call arg ---");
    consume_tracked(identity_tracked(make_tracked("nested")));
    println("after consume(identity(make()))");

    println("--- orphaned fn call ---");
    make_tracked("orphan-fn");
    println("after orphaned make()");

    println("--- orphaned construct expr ---");
    TrackedVar{"orphan-construct"};
    println("after orphaned TrackedVar construct");

    println("--- if expr as fn arg ---");
    consume_tracked(true ? make_tracked("if-true") : make_tracked("if-false"));
    println("after consume(if ...)");

    // Regression: early return BEFORE a later stmt that owns a stmt-temp must
    // not destroy the temp's alloca (it's live but uninitialized). Historically
    // crashed on Linux and silently read garbage on macOS.
    println("--- early return before later stmt-temp ---");
    early_return_before_temp(true);
    println("after early(true)");
    early_return_before_temp(false);
    println("after early(false)");
}

func early_return_before_temp(fast: bool) {
    if fast {
        return;
    }
    consume_tracked(make_tracked("late-temp"));
}

func test_paren_temp_cleanup() {
    println("=== Test 10b: ParenExpr temp cleanup ===");

    println("--- orphaned (fn call) ---");
    (make_tracked("paren-orphan"));
    println("after orphaned (make())");

    println("--- (fn call) as arg ---");
    consume_tracked((make_tracked("paren-arg")));
    println("after consume((make()))");

    println("--- nested ((fn call)) ---");
    consume_tracked((((make_tracked("paren-nested")))));
    println("after consume(((make())))");

    println("--- (if expr) as arg ---");
    consume_tracked((true ? make_tracked("paren-if-t") : make_tracked("paren-if-f")));
    println("after consume((if...))");
}

func consume_tuple(t: Tuple<int, TrackedVar>) {}

func make_tuple_traced(name: string) Tuple<int, TrackedVar> {
    return (1, make_tracked(name));
}

func test_tuple_temp_cleanup() {
    println("=== Test 10c: TupleExpr temp cleanup ===");

    println("--- orphaned tuple with destructible element ---");
    (1, make_tracked("tuple-orphan"));
    println("after orphaned tuple");

    println("--- tuple as arg ---");
    consume_tuple((2, make_tracked("tuple-arg")));
    println("after consume(tuple)");

    println("--- nested orphan tuple ---");
    (1, (2, make_tracked("tuple-nested")));
    println("after nested orphan");

    println("--- returned tuple bound to var ---");
    let t = make_tuple_traced("tuple-return");
    printf("  t.1.name='{}'\n", t.1.name);
    println("after return binding");
}

func test_expr_contexts() {
    println("=== Test 11: Temps in expression-position contexts ===");

    // if condition: DotExpr base on temp — temp owned and destroyed at scope exit
    println("--- if condition: DotExpr base on temp ---");
    if make_tracked("if-cond").name == "if-cond" {
        println("if matched");
    }
    println("after if");

    // while condition: DotExpr base on temp — single iteration via break
    println("--- while condition: DotExpr base on temp ---");
    while make_tracked("while-cond").name == "while-cond" {
        break;
    }
    println("after while");

    // for loop: orphaned temp in post position — destroyed at scope exit
    println("--- for post: orphaned temp ---");
    for var i: uint32 = 0; i < 1; make_tracked("for-post") {
        println("in for loop");
        i = i + 1;
    }
    println("after for");

    // unary op: temp as operand — owned at scope
    println("--- unary op on temp ---");
    if !make_tracked("unary").name.is_empty() {
        println("unary not-empty matched");
    }
    println("after unary");
}

struct TrackedContainer {
    inner: TrackedVar;

    mut func new(inner: TrackedVar) {
        this.inner = inner;
    }
}

func consume_two(a: TrackedVar, b: TrackedVar) {
    printf("  consumed '{}' and '{}'\n", a.name, b.name);
}

func test_fn_arg_copy_semantics() {
    println("=== Test 12: Function arg copy semantics ===");

    // Named var to fn: caller copies, callee owns the copy
    println("--- named var to fn ---");
    var t = TrackedVar{"named"};
    consume_tracked(t);
    printf("  after consume: t.name='{}'\n", t.name);

    // move(x) to fn: no copy, ownership transfers
    println("--- move to fn ---");
    var t2 = TrackedVar{"moved"};
    consume_tracked(move (t2));
    println("  after consume(move)");

    // Named var to constructor: caller copies for constructor param
    println("--- named var to constructor ---");
    var t3 = TrackedVar{"ctor-named"};
    var c = TrackedContainer{t3};
    printf("  c.inner.name='{}', t3.name='{}'\n", c.inner.name, t3.name);

    // Rvalue to constructor: no copy for constructor param (moved)
    println("--- rvalue to constructor ---");
    var c2 = TrackedContainer{make_tracked("ctor-rvalue")};
    printf("  c2.inner.name='{}'\n", c2.inner.name);

    // move(x) to constructor: no copy
    println("--- move to constructor ---");
    var t4 = TrackedVar{"ctor-moved"};
    var c3 = TrackedContainer{move (t4)};
    printf("  c3.inner.name='{}'\n", c3.inner.name);
    println("  after move-construct");

    // Multiple destructible args: both properly copied
    println("--- multiple args ---");
    var a = TrackedVar{"arg-a"};
    var b = TrackedVar{"arg-b"};
    consume_two(a, b);
    printf("  after: a='{}', b='{}'\n", a.name, b.name);

    // Multiple rvalue args: both moved, no copies
    println("--- multiple rvalue args ---");
    consume_two(make_tracked("rv-a"), make_tracked("rv-b"));
    println("  after consume_two(make(), make())");

    // Named var in loop: alloca reuse, correct copies each iteration
    println("--- named var in loop ---");
    for var i: uint32 = 0; i < 2; i++ {
        var h = TrackedVar{i == 0 ? "loop-0" : "loop-1"};
        consume_tracked(h);
    }
    println("  after loop");

    println("--- scope exit ---");
}

// TrackedVal: like TrackedVar but with Add operator for testing operator method cleanup
struct TrackedVal {
    id: int = 0;

    mut func new(id: int) {
        this.id = id;
        printf("  TrackedVal.new({})\n", id);
    }

    mut func delete() {
        printf("  TrackedVal.delete({})\n", this.id);
    }

    impl ops.Copy {
        mut func copy(source: &This) {
            this.id = source.id;
            printf("  TrackedVal.copy({})\n", source.id);
        }
    }

    impl ops.Add {
        func add(rhs: &This) TrackedVal {
            printf("  TrackedVal.add({}, {})\n", this.id, rhs.id);
            return {this.id + rhs.id};
        }
    }

    // Regular method with by-value param (same signature as add)
    func combine(other: TrackedVal) TrackedVal {
        printf("  TrackedVal.combine({}, {})\n", this.id, other.id);
        return {this.id + other.id};
    }

    // Method that consumes param without returning
    func absorb(other: TrackedVal) {
        printf("  TrackedVal.absorb({}, {})\n", this.id, other.id);
    }

    // Method with multiple by-value params
    func merge(a: TrackedVal, b: TrackedVal) TrackedVal {
        printf("  TrackedVal.merge({}, {}, {})\n", this.id, a.id, b.id);
        return {this.id + a.id + b.id};
    }
}

func test_method_param_cleanup() {
    println("=== Test 13: Method by-value param cleanup ===");

    // Regular method: by-value param destructed at method exit
    println("--- regular method ---");
    var a = TrackedVal{10};
    var b = TrackedVal{5};
    var c = a.combine(b);
    printf("  c.id={}\n", c.id);

    // Operator method: rhs param destructed at method exit
    println("--- operator method (a + b) ---");
    var d = TrackedVal{20};
    var e = TrackedVal{7};
    var f = d + e;
    printf("  f.id={}\n", f.id);

    // Void method: by-value param destructed at method exit
    println("--- void method ---");
    var g = TrackedVal{30};
    var h = TrackedVal{8};
    g.absorb(h);
    printf("  after absorb\n");

    // Multiple by-value params on method
    println("--- multi-param method ---");
    var i = TrackedVal{1};
    var j = TrackedVal{2};
    var k = TrackedVal{3};
    var l = i.merge(j, k);
    printf("  l.id={}\n", l.id);

    // Compound assignment with operator method
    println("--- compound assignment (+=) ---");
    var m = TrackedVal{100};
    var n = TrackedVal{50};
    m += n;
    printf("  m.id={}\n", m.id);

    // Chained: a + b + c (intermediate temps)
    println("--- chained operators ---");
    var p = TrackedVal{1};
    var q = TrackedVal{2};
    var r = TrackedVal{3};
    var s = p + q + r;
    printf("  s.id={}\n", s.id);

    // Rvalue to method: no caller-side copy, param still cleaned up
    println("--- rvalue to method ---");
    var t = TrackedVal{40};
    var u = t.combine({9});
    printf("  u.id={}\n", u.id);

    println("--- scope exit ---");
}

func make_val(id: int) TrackedVal {
    return {id};
}

struct PairVal {
    first: TrackedVal;
    second: TrackedVal;
}

func make_pair_val(a: int, b: int) PairVal {
    return {first: {a}, second: {b}};
}

func make_tuple_val(a: int, b: int) Tuple<TrackedVal, TrackedVal> {
    return (TrackedVal{a}, TrackedVal{b});
}

func make_optional_pair_val(a: int, b: int) ?PairVal {
    return make_pair_val(a, b);
}

func consume_val(value: TrackedVal) {
    printf("  consumed.id={}\n", value.id);
}

func unwrap_projected_val() TrackedVal {
    return make_optional_pair_val(16, 17)!.first;
}

struct HoldsVal {
    data: TrackedVal;

    mut func new(id: int) {
        this.data = make_val(id);
    }
}

struct HoldsValArray {
    items: Array<TrackedVal> = [];
}

func make_holds_val_array(a: int, b: int) HoldsValArray {
    var items: Array<TrackedVal> = [];
    items.push({a});
    items.push({b});
    return {:items};
}

func test_temp_move_semantics() {
    println("=== Test 14: Temp move semantics (no unnecessary copies) ===");

    // VarDecl from fn call: temp moved, not copied
    println("--- vardecl from fn call ---");
    var a = make_val(1);
    printf("  a.id={}\n", a.id);

    // VarDecl from construct expr: temp moved, not copied
    println("--- vardecl from construct ---");
    var b = TrackedVal{2};
    printf("  b.id={}\n", b.id);

    // Assignment from fn call: old destructed, temp moved
    println("--- reassign from fn call ---");
    var c = TrackedVal{3};
    c = make_val(4);
    printf("  c.id={}\n", c.id);

    // Assignment from construct: old destructed, temp moved
    println("--- reassign from construct ---");
    var d = TrackedVal{5};
    d = {6};
    printf("  d.id={}\n", d.id);

    // Struct field from fn call in constructor: temp moved
    println("--- struct field from fn call ---");
    var h = HoldsVal{7};
    printf("  h.data.id={}\n", h.data.id);

    // Projection from temp struct: copied, not moved from partial owner
    println("--- field projection from temp ---");
    var p = make_pair_val(8, 9).first;
    printf("  p.id={}\n", p.id);

    // Projection from temp tuple: copied, not moved from partial owner
    println("--- tuple projection from temp ---");
    var q = make_tuple_val(10, 11).0;
    printf("  q.id={}\n", q.id);

    // Optional unwrap projection from temp: copied, not moved from partial owner
    println("--- optional unwrap projection from temp ---");
    var r = make_optional_pair_val(12, 13)!.first;
    printf("  r.id={}\n", r.id);

    println("--- optional unwrap projection to arg ---");
    consume_val(make_optional_pair_val(14, 15)!.first);

    println("--- optional unwrap projection to return ---");
    var s = unwrap_projected_val();
    printf("  s.id={}\n", s.id);

    // Lambda vardecl and reassign: captures not leaked
    println("--- lambda lifecycle ---");
    var x: int = 10;
    var f1 = func [x] () int {
        return x;
    };
    printf("  f1()={}\n", f1());
    var f2 = f1;
    printf("  f1()={}, f2()={}\n", f1(), f2());
    f1 = func () int {
        return 99;
    };
    printf("  f1()={}, f2()={}\n", f1(), f2());

    println("--- scope exit ---");
}

func test_map_set_replacement_cleanup() {
    println("=== Test 15: Map.set replacement destroys old value ===");
    var m = Map<int, TrackedVal>{};
    m.set(1, {201});
    m.set(1, {202});
    println("--- scope exit ---");
}

func consume_holds_val_array(value: HoldsValArray) {
    printf("  consumed.len={}\n", value.items.length);
}

func test_by_value_nested_array_param_cleanup() {
    println("=== Test 16: by-value nested array param owns its own copy ===");
    var value = make_holds_val_array(211, 212);
    consume_holds_val_array(value);
    println("--- scope exit ---");
}

func test_array_copy_nested_array_cleanup() {
    println("=== Test 17: Array.copy handles nested array values ===");
    var values: Array<HoldsValArray> = [];
    values.push(make_holds_val_array(301, 302));
    values.push(make_holds_val_array(303, 304));
    var copied = values;
    printf("  copied.len={}\n", copied.length);
    println("--- scope exit ---");
}

struct TrackedInitField {
    id: int = 0;

    mut func new(id: int) {
        this.id = id;
        printf("  TrackedInitField.new({})\n", id);
    }

    mut func delete() {
        printf("  TrackedInitField.delete({})\n", this.id);
    }

    impl ops.Copy {
        mut func copy(source: &This) {
            this.id = source.id;
        }
    }
}

struct HoldsTrackedInitField {
    item: TrackedInitField = {410};
}

func test_construct_field_initializer_cleanup() {
    println("=== Test 18: explicit field init destroys default field value ===");
    var value = HoldsTrackedInitField{item: {420}};
    printf("  value.item.id={}\n", value.item.id);
    println("--- scope exit ---");
}

struct HoldsTrackedNoDefaultField {
    item: TrackedInitField;
}

func test_construct_field_initializer_without_default() {
    println("=== Test 19: explicit field init without default does not destroy garbage ===");
    var value = HoldsTrackedNoDefaultField{item: {430}};
    printf("  value.item.id={}\n", value.item.id);
    println("--- scope exit ---");
}

func accept_val(v: TrackedVal) {
    printf("  accepted.id={}\n", v.id);
}

func maybe_move_param(v: TrackedVal, do_move: bool) {
    if do_move {
        accept_val(move v);
    }
    println("  after maybe_move");
}

func test_maybe_moved_param() {
    println("=== Test 20: Maybe-moved parameter drop flag ===");

    println("--- param moved ---");
    maybe_move_param({501}, true);
    println("  returned");

    println("--- param not moved ---");
    maybe_move_param({502}, false);
    println("  returned");
}

func test_maybe_moved_for_bind() {
    println("=== Test 21: Maybe-moved for-loop bind drop flag ===");

    println("--- fixed array ---");
    var arr: [3]TrackedVal = [{601}, {602}, {603}];
    for item in arr {
        if item.id == 602 {
            accept_val(move item);
        }
    }
    println("  after fixed array loop");

    println("--- dynamic array ---");
    var dyn: Array<TrackedVal> = [];
    dyn.push({701});
    dyn.push({702});
    dyn.push({703});
    for item in dyn {
        if item.id == 702 {
            accept_val(move item);
        }
    }
    println("  after dynamic array loop");

    println("--- scope exit ---");
}

struct ValueBox<T> {
    value: T;

    func transform<U>(f: func (value: T) U) ValueBox<U> {
        return {value: f(this.value)};
    }
}

func test_lambda_call_in_struct_literal() {
    println("=== Test 22: lambda call result in struct literal does not leak ===");
    var box = ValueBox<int>{value: 7};
    var result = box.transform(func (v: int) TrackedVal {
        return {900 + v};
    });
    printf("  result.value.id={}\n", result.value.id);
    println("--- scope exit ---");
}

func test_then_lambda_capture_destroyed() {
    println("=== Test 23: Promise.then lambda captures are destroyed ===");
    var p = Promise<int>{};
    p.then(func (val: int) TrackedVal {
        return {900};
    });
    p.resolve(42);
    println("--- scope exit ---");
}

func main() {
    test_auto_destroy_no_custom_delete();
    test_new_initializes_defaults();
    test_destruction_order();
    test_shared_auto_destroy();
    test_nested_destruction();
    test_optional_auto_destroy();
    test_optional_direct();
    test_both_lifecycles();
    test_multiple_vars();
    test_temp_cleanup();
    test_paren_temp_cleanup();
    test_tuple_temp_cleanup();
    test_expr_contexts();
    test_fn_arg_copy_semantics();
    test_method_param_cleanup();
    test_temp_move_semantics();
    test_map_set_replacement_cleanup();
    test_by_value_nested_array_param_cleanup();
    test_array_copy_nested_array_cleanup();
    test_construct_field_initializer_cleanup();
    test_construct_field_initializer_without_default();
    test_maybe_moved_param();
    test_maybe_moved_for_bind();
    test_lambda_call_in_struct_literal();
    test_then_lambda_capture_destroyed();
    println("All tests completed!");
}
