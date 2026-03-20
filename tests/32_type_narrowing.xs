import "std/ops" as ops;

// === Basic narrowing (single variable) ===

func basic_positive(x: ?int) int {
    if x {
        return x + 1;
    }
    return -1;
}

func basic_guard(x: ?int) int {
    if !x {
        return -1;
    }
    return x + 1;
}

func basic_assert(x: ?int) int {
    assert(x);
    return x + 1;
}

// === Compound && narrowing ===

func and_positive(a: ?int, b: ?int) int {
    if a && b {
        return a + b;
    }
    return -1;
}

func and_positive_mixed(a: ?int, b: ?string) int {
    if a && b {
        return a + b.length;
    }
    return -1;
}

func and_three(a: ?int, b: ?int, c: ?int) int {
    if a && b && c {
        return a + b + c;
    }
    return -1;
}

// === Compound || guard narrowing ===

func or_guard(a: ?int, b: ?int) int {
    if !a || !b {
        return -1;
    }
    return a + b;
}

func or_guard_three(a: ?int, b: ?int, c: ?int) int {
    if !a || !b || !c {
        return -1;
    }
    return a + b + c;
}

// === Negated && guard: !(a && b) ===

func negated_and_guard(a: ?int, b: ?int) int {
    if !(a && b) {
        return -1;
    }
    return a + b;
}

func negated_and_guard_three(a: ?int, b: ?int, c: ?int) int {
    if !(a && b && c) {
        return -1;
    }
    return a + b + c;
}

// === Assert compound ===

func assert_and(a: ?int, b: ?int) int {
    assert(a && b);
    return a + b;
}

func assert_and_three(a: ?int, b: ?int, c: ?int) int {
    assert(a && b && c);
    return a + b + c;
}

// === Mixed with non-optional bool conditions ===

func and_with_bool(x: ?int, flag: bool) int {
    if x && flag {
        return x * 2;
    }
    return -1;
}

func guard_or_with_bool(x: ?int, flag: bool) int {
    if !x || !flag {
        return -1;
    }
    return x * 2;
}

// === Chained guards (sequential if-guards) ===

func sequential_guards(a: ?int, b: ?int) int {
    if !a {
        return -1;
    }
    if !b {
        return -2;
    }
    return a + b;
}

// === && inside if-else (narrowing in then-block only) ===

func and_else_branch(a: ?int, b: ?int) string {
    if a && b {
        return stringf("sum={}", a + b);
    } else {
        return "missing";
    }
}

// === Double negation ===

func double_negation(x: ?int) int {
    // !!x is truthy when x is truthy
    if !!x {
        return x + 1;
    }
    return -1;
}

// === Guard with early return in block ===

func guard_block(a: ?int, b: ?int) int {
    if !a || !b {
        printf("  (guard: returning -1)\n");
        return -1;
    }
    return a + b;
}

// === Narrowing with string operations ===

func narrow_string(name: ?string, greeting: ?string) string {
    if name && greeting {
        return greeting + " " + name;
    }
    return "???";
}

// === && presence semantics (Optional as bool in &&) ===

func truthiness_and(a: ?int, b: ?int) bool {
    return a && b;
}

func truthiness_or(a: ?int, b: ?int) bool {
    return a || b;
}

// === Narrowing alias correctness with destructible types ===

struct Traced {
    id: int = 0;

    mut func new(id: int) {
        this.id = id;
        printf("  construct({})\n", id);
    }

    mut func delete() {
        printf("  destroy({})\n", this.id);
    }

    impl ops.Copy {
        mut func copy(source: &This) {
            this.id = source.id;
        }
    }
}

// Positive narrowing: read-only (no copy, no extra destroy)
func narrow_read_only() {
    var x: ?Traced = Traced{1};
    if x {
        printf("  read: {}\n", x.id);
    }
}

// Positive narrowing: mutable reassignment through alias
// Previously broken: double-free of old value + leak of new value
func narrow_reassign() {
    var x: ?Traced = Traced{10};
    if x {
        printf("  before: {}\n", x.id);
        x = Traced{20};
        printf("  after: {}\n", x.id);
    }
}

// Guard clause narrowing: read-only
func narrow_guard_read() {
    var x: ?Traced = Traced{100};
    if !x {
        return;
    }
    printf("  guard read: {}\n", x.id);
}

// Guard clause narrowing: mutable reassignment
func narrow_guard_reassign() {
    var x: ?Traced = Traced{200};
    if !x {
        return;
    }
    printf("  before: {}\n", x.id);
    x = Traced{300};
    printf("  after: {}\n", x.id);
}

// Assert narrowing: mutable reassignment
func narrow_assert_reassign() {
    var x: ?Traced = Traced{400};
    assert(x);
    printf("  before: {}\n", x.id);
    x = Traced{500};
    printf("  after: {}\n", x.id);
}

// Compound narrowing with destructible types
func narrow_compound() {
    var a: ?Traced = Traced{1000};
    var b: ?Traced = Traced{2000};
    if a && b {
        printf("  a={} b={}\n", a.id, b.id);
    }
}

// Null case: no construct/destroy for the value
func narrow_null_case() {
    var x: ?Traced = null;
    if x {
        printf("  should not print\n");
    }
    printf("  null case done\n");
}

// === DotExpr (field access) narrowing ===

struct Container {
    value: ?int = null;
    name: ?string = null;
}

struct App {
    service: ?Traced = null;

    func test_positive() {
        if this.service {
            printf("  positive: {}\n", this.service.id);
        } else {
            println("  positive: null");
        }
    }

    func test_guard() {
        if !this.service {
            println("  guard: null");
            return;
        }
        printf("  guard: {}\n", this.service.id);
    }

    func test_assert() {
        assert(this.service);
        printf("  assert: {}\n", this.service.id);
    }
}

func dot_positive() {
    var c = Container{};
    c.value = 42;
    if c.value {
        printf("dot_positive={}\n", c.value + 1);
    }
}

func dot_guard() {
    var c = Container{};
    if !c.value {
        println("dot_guard=null");
        return;
    }
    printf("dot_guard={}\n", c.value + 1);
}

func dot_assert() {
    var c = Container{};
    c.value = 99;
    assert(c.value);
    printf("dot_assert={}\n", c.value + 1);
}

func dot_compound() {
    var c = Container{};
    c.value = 10;
    c.name = "hello";
    if c.value && c.name {
        printf("dot_compound={} {}\n", c.value, c.name);
    }
}

func dot_compound_guard() {
    var c = Container{};
    c.value = 5;
    c.name = "world";
    if !c.value || !c.name {
        println("dot_compound_guard=null");
        return;
    }
    printf("dot_compound_guard={} {}\n", c.value, c.name);
}

func dot_this_positive() {
    var app = App{};
    app.service = Traced{42};
    println("positive:");
    app.test_positive();
}

func dot_this_guard() {
    var app = App{};
    app.service = Traced{100};
    println("guard:");
    app.test_guard();
}

func dot_this_null() {
    var app = App{};
    println("null positive:");
    app.test_positive();
    println("null guard:");
    app.test_guard();
}

func dot_this_assert() {
    var app = App{};
    app.service = Traced{200};
    println("assert:");
    app.test_assert();
}

func main() {
    println("-- Basic narrowing --");
    printf("basic_positive(5)={}\n", basic_positive(5));
    printf("basic_positive(null)={}\n", basic_positive(null));
    printf("basic_guard(5)={}\n", basic_guard(5));
    printf("basic_guard(null)={}\n", basic_guard(null));
    printf("basic_assert(5)={}\n", basic_assert(5));

    println("\n-- Compound && positive --");
    printf("and_positive(1,2)={}\n", and_positive(1, 2));
    printf("and_positive(null,2)={}\n", and_positive(null, 2));
    printf("and_positive(1,null)={}\n", and_positive(1, null));
    printf("and_positive(null,null)={}\n", and_positive(null, null));
    printf("and_positive_mixed(3,\"hello\")={}\n", and_positive_mixed(3, "hello"));
    printf("and_positive_mixed(null,\"hello\")={}\n", and_positive_mixed(null, "hello"));
    printf("and_three(1,2,3)={}\n", and_three(1, 2, 3));
    printf("and_three(1,null,3)={}\n", and_three(1, null, 3));

    println("\n-- Compound || guard --");
    printf("or_guard(3,4)={}\n", or_guard(3, 4));
    printf("or_guard(null,4)={}\n", or_guard(null, 4));
    printf("or_guard(3,null)={}\n", or_guard(3, null));
    printf("or_guard_three(1,2,3)={}\n", or_guard_three(1, 2, 3));
    printf("or_guard_three(null,2,3)={}\n", or_guard_three(null, 2, 3));

    println("\n-- Negated && guard --");
    printf("negated_and_guard(5,6)={}\n", negated_and_guard(5, 6));
    printf("negated_and_guard(null,6)={}\n", negated_and_guard(null, 6));
    printf("negated_and_guard(5,null)={}\n", negated_and_guard(5, null));
    printf("negated_and_guard_three(1,2,3)={}\n", negated_and_guard_three(1, 2, 3));
    printf("negated_and_guard_three(1,null,3)={}\n", negated_and_guard_three(1, null, 3));

    println("\n-- Assert compound --");
    printf("assert_and(10,20)={}\n", assert_and(10, 20));
    printf("assert_and_three(1,2,3)={}\n", assert_and_three(1, 2, 3));

    println("\n-- Mixed with bool --");
    printf("and_with_bool(5,true)={}\n", and_with_bool(5, true));
    printf("and_with_bool(5,false)={}\n", and_with_bool(5, false));
    printf("and_with_bool(null,true)={}\n", and_with_bool(null, true));
    printf("guard_or_with_bool(5,true)={}\n", guard_or_with_bool(5, true));
    printf("guard_or_with_bool(5,false)={}\n", guard_or_with_bool(5, false));
    printf("guard_or_with_bool(null,true)={}\n", guard_or_with_bool(null, true));

    println("\n-- Sequential guards --");
    printf("sequential_guards(1,2)={}\n", sequential_guards(1, 2));
    printf("sequential_guards(null,2)={}\n", sequential_guards(null, 2));
    printf("sequential_guards(1,null)={}\n", sequential_guards(1, null));

    println("\n-- And with else branch --");
    printf("and_else_branch(3,7)={}\n", and_else_branch(3, 7));
    printf("and_else_branch(null,7)={}\n", and_else_branch(null, 7));

    println("\n-- Double negation --");
    printf("double_negation(5)={}\n", double_negation(5));
    printf("double_negation(null)={}\n", double_negation(null));

    println("\n-- Guard with block --");
    printf("guard_block(3,4)={}\n", guard_block(3, 4));
    printf("guard_block(null,4)={}\n", guard_block(null, 4));

    println("\n-- String narrowing --");
    printf("narrow_string(\"World\",\"Hello\")={}\n", narrow_string("World", "Hello"));
    printf("narrow_string(null,\"Hello\")={}\n", narrow_string(null, "Hello"));
    printf("narrow_string(\"World\",null)={}\n", narrow_string("World", null));

    println("\n-- Presence operators --");
    printf("truthiness_and(1,2)={}\n", truthiness_and(1, 2));
    printf("truthiness_and(0,2)={}\n", truthiness_and(0, 2));
    printf("truthiness_and(null,2)={}\n", truthiness_and(null, 2));
    printf("truthiness_and(1,null)={}\n", truthiness_and(1, null));
    printf("truthiness_or(1,2)={}\n", truthiness_or(1, 2));
    printf("truthiness_or(0,null)={}\n", truthiness_or(0, null));
    printf("truthiness_or(null,2)={}\n", truthiness_or(null, 2));
    printf("truthiness_or(null,null)={}\n", truthiness_or(null, null));

    println("\n-- Narrowing alias: destructible types --");
    println("read_only:");
    narrow_read_only();
    println("reassign:");
    narrow_reassign();
    println("guard_read:");
    narrow_guard_read();
    println("guard_reassign:");
    narrow_guard_reassign();
    println("assert_reassign:");
    narrow_assert_reassign();
    println("compound:");
    narrow_compound();
    println("null_case:");
    narrow_null_case();

    println("\n-- DotExpr narrowing --");
    dot_positive();
    dot_guard();
    dot_assert();
    dot_compound();
    dot_compound_guard();
    dot_this_positive();
    dot_this_guard();
    dot_this_null();
    dot_this_assert();
}
