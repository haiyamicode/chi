import "std/ops" as ops;

struct Inner1 {
    id: int = 0;

    func new(id: int) {
        this.id = id;
        printf("  Inner1.new({})\n", id);
    }

    func delete() {
        printf("  Inner1.delete({})\n", this.id);
    }
}

struct Outer1 {
    inner: Inner1;

    func new(id: int) {
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

    func new(c_val: int) {
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

    func new(name: string) {
        this.name = name;
        printf("  OrderedField.new('{}')\n", name);
    }

    func delete() {
        printf("  OrderedField.delete('{}')\n", this.name);
    }
}

struct OrderedContainer {
    first: OrderedField;
    second: OrderedField;
    third: OrderedField;

    func new() {
        println("OrderedContainer.new");
        this.first = {"first"};
        this.second = {"second"};
        this.third = {"third"};
    }

    func delete() {
        println("OrderedContainer.delete (user)");
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

    func new(v: int) {
        this.value = v;
        printf("  RefCountedData.new({})\n", v);
    }

    func delete() {
        printf("  RefCountedData.delete({})\n", this.value);
    }
}

struct HoldsShared {
    data: Shared<RefCountedData>;

    func new(v: int) {
        println("HoldsShared.new");
        this.data = {{v}};
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

    func delete() {
        printf("    Level3.delete({})\n", this.id);
    }
}

struct Level2 {
    id: int = 0;
    child: Level3;

    func new(id: int) {
        this.id = id;
        this.child = {id: id * 10};
    }

    func delete() {
        printf("  Level2.delete({})\n", this.id);
    }
}

struct Level1 {
    id: int = 0;
    child: Level2;

    func new(id: int) {
        this.id = id;
        this.child = {id * 10};
    }

    func delete() {
        printf("Level1.delete({})\n", this.id);
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

    func new(v: int) {
        this.value = v;
        printf("  OptionalData.new({})\n", v);
    }

    func delete() {
        printf("  OptionalData.delete({})\n", this.value);
    }
}

struct HoldsOptionalShared {
    data: ?Shared<OptionalData> = null;

    func new(v: int) {
        println("HoldsOptionalShared.new");
        this.data! = {{v}};
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

    func new(id: int) {
        this.id = id;
        printf("  DirectStruct.new({})\n", id);
    }

    func delete() {
        printf("  DirectStruct.delete({})\n", this.id);
    }
}

struct HoldsOptionalDirect {
    data: ?DirectStruct = null;

    func new(id: int) {
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

    func new(id: int) {
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

    func new(name: string) {
        this.name = name;
        printf("  TrackedVar.new('{}')\n", name);
    }

    func delete() {
        printf("  TrackedVar.delete('{}')\n", this.name);
    }

    impl ops.CopyFrom<TrackedVar> {
        func copy_from(source: &TrackedVar) {
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

func consume_tracked(t: TrackedVar) {
}

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
    consume_tracked(if true => make_tracked("if-true") else => make_tracked("if-false"));
    println("after consume(if ...)");
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
    test_expr_contexts();
    println("All tests completed!");
}

