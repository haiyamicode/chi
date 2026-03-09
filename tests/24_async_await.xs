import "std/ops" as ops;
import "std/time" as time;

// --- Promise API ---

func test_manual_promise() {
    println("=== Manual Promise ===");
    var p = Promise<int>{};
    p.then(func (val: int) {
        printf("Callback: {}\n", val);
    });
    println("Resolving...");
    p.resolve(100);

    // then on already-resolved promise
    var p2 = Promise<int>{};
    p2.resolve(200);
    p2.then(func (val: int) {
        printf("Already resolved: {}\n", val);
    });
}

func test_promise_state() {
    println("=== Promise state ===");
    var p = Promise<int>{};
    printf("before: {}\n", p.is_resolved());
    p.resolve(42);
    printf("after: {}\n", p.is_resolved());
    printf("value: {}\n", p.value()!);
}

func test_then_chain() {
    println("=== Then chain ===");
    // Multi-step chain with type changes: int -> string -> int -> void
    var p = Promise<int>{};
    var p2 = p.then(func (v: int) string {
        return stringf("n={}", v);
    });
    var p3 = p2.then(func (s: string) int {
        return s.length;
    });
    var p4 = p3.then(func (v: int) {
        printf("chain result: {}\n", v);
    });
    printf(
        "before resolve: {} {} {} {}\n",
        p.is_resolved(),
        p2.is_resolved(),
        p3.is_resolved(),
        p4.is_resolved()
    );
    p.resolve(42);
    printf(
        "after resolve: {} {} {} {}\n",
        p.is_resolved(),
        p2.is_resolved(),
        p3.is_resolved(),
        p4.is_resolved()
    );
    printf("p2: '{}'\n", p2.value()!);
    printf("p3: {}\n", p3.value()!);

    // Chain on already-resolved promise
    var r = Promise<int>{};
    r.resolve(7);
    var r2 = r.then(func (v: int) int {
        return v * 3;
    });
    var r3 = r2.then(func (v: int) string {
        return stringf("got {}", v);
    });
    printf("already resolved chain: '{}'\n", r3.value()!);
}

func test_promise_make() {
    println("=== Promise.make ===");
    var p = Promise<int>.make(func (resolve) {
        resolve(123);
    });
    printf("value: {}\n", p.value()!);
}

func test_promise_string() {
    println("=== Promise<string> ===");
    var p1 = Promise<string>.make(func (resolve) {
        resolve("hello world");
    });
    printf("p1: '{}'\n", p1.value()!);

    var prefix = "hello";
    var p2 = Promise<string>.make(
        func [prefix] (resolve) {
            resolve(prefix + " from capture");
        }
    );
    printf("p2: '{}'\n", p2.value()!);

    var p3 = Promise<string>{};
    p3.then(func (v: string) {
        printf("deferred: '{}'\n", v);
    });
    p3.resolve("resolved later");
}

struct Container {
    static func make_promise(msg: string) Promise<string> {
        return Promise<string>.make(func [msg] (resolve) {
            resolve(msg);
        });
    }
}

func test_promise_in_struct() {
    println("=== Promise in struct ===");
    var p = Container.make_promise("from struct");
    printf("value: '{}'\n", p.value()!);
}

// --- Async/await helpers ---

async func double_it(x: int) Promise<int> {
    return x * 2;
}

async func add_async(a: int, b: int) Promise<int> {
    return a + b;
}

async func greet(name: string) Promise<string> {
    return "hello " + name;
}

// --- Async no await ---

async func simple_async(x: int) Promise<int> {
    return x + 10;
}

func test_async_no_await() {
    println("=== Async no await ===");
    var p = simple_async(7);
    printf("value: {}\n", p.value()!);
}

// --- Await: var decl ---

async func do_var_decl() Promise<int> {
    var x = await double_it(5);
    return x;
}

func test_var_decl() {
    println("=== Var decl ===");
    printf("value: {}\n", do_var_decl().value()!);
}

// --- Await: bare ---

async func do_bare_await() Promise {
    await time.sleep(1);
    println("bare ok");
}

func test_bare_await() {
    println("=== Bare await ===");
    do_bare_await();
}

// --- Await: func arg ---

async func do_await_in_func_arg() Promise<int> {
    var result = await double_it(7);
    printf("in func arg: {}\n", result);
    return result;
}

func test_await_in_func_arg() {
    println("=== Await in func arg ===");
    printf("value: {}\n", do_await_in_func_arg().value()!);
}

// --- Await: binop ---

async func do_await_in_binop() Promise<int> {
    var x = await double_it(3) + 100;
    return x;
}

func test_await_in_binop() {
    println("=== Await in binop ===");
    printf("value: {}\n", do_await_in_binop().value()!);
}

// --- Await: multiple sequential ---

async func do_multiple_awaits() Promise<int> {
    var a = await double_it(1);
    var b = await double_it(2);
    var c = await double_it(3);
    return a + b + c;
}

func test_multiple_awaits() {
    println("=== Multiple awaits ===");
    printf("value: {}\n", do_multiple_awaits().value()!);
}

// --- Await: locals across boundaries ---

async func do_locals_across() Promise<int> {
    var before = 42;
    var x = await double_it(10);
    return before + x;
}

func test_locals_across() {
    println("=== Locals across await ===");
    printf("value: {}\n", do_locals_across().value()!);
}

// --- Await: chain ---

async func compute_chain(start: int) Promise<int> {
    var step1 = await double_it(start);
    var step2 = await double_it(step1);
    var step3 = await double_it(step2);
    return step3;
}

func test_chain() {
    println("=== Chain ===");
    printf("value: {}\n", compute_chain(5).value()!);
}

// --- Await: nested async ---

async func inner_chain(x: int) Promise<int> {
    var a = await double_it(x);
    return a + 1;
}

async func do_nested_chain() Promise<int> {
    var result = await inner_chain(10);
    return result;
}

func test_nested_chain() {
    println("=== Nested chain ===");
    printf("value: {}\n", do_nested_chain().value()!);
}

// --- Await: return await ---

async func do_return_await() Promise<int> {
    return await double_it(25);
}

func test_return_await() {
    println("=== Return await ===");
    printf("value: {}\n", do_return_await().value()!);
}

// --- Await: comparison ---

async func do_await_comparison() Promise<bool> {
    var big = await double_it(5) > 8;
    return big;
}

func test_await_comparison() {
    println("=== Await in comparison ===");
    printf("value: {}\n", do_await_comparison().value()!);
}

// --- Await: interleaved with non-await stmts ---

async func do_interleaved() Promise<int> {
    var a = await double_it(1);
    var b = await double_it(a);
    var c = await double_it(b);
    return a + b + c;
}

func test_interleaved() {
    println("=== Interleaved ===");
    printf("value: {}\n", do_interleaved().value()!);
}

// --- Await: string ---

async func do_await_string() Promise<string> {
    var msg = await greet("world");
    return msg;
}

func test_await_string() {
    println("=== Await string ===");
    printf("value: '{}'\n", do_await_string().value()!);
}

// --- Await: string concat ---

async func do_string_concat() Promise<string> {
    var msg = await greet("world") + "!";
    return msg;
}

func test_string_concat() {
    println("=== String concat ===");
    printf("value: '{}'\n", do_string_concat().value()!);
}

// --- Await: as last stmt (no code after) ---

async func do_await_last() Promise<int> {
    var x = await double_it(99);
    return x;
}

func test_await_last() {
    println("=== Await as last ===");
    printf("value: {}\n", do_await_last().value()!);
}

// --- Await: double nested chain ---

async func double_chain(x: int) Promise<int> {
    var step1 = await double_it(x);
    var step2 = await double_it(step1);
    return step2;
}

async func do_double_chain() Promise<int> {
    return await double_chain(3);
}

func test_double_chain() {
    println("=== Double chain ===");
    printf("value: {}\n", do_double_chain().value()!);
}

// --- Await: bare then logic ---

async func do_bare_then_logic() Promise {
    var x = 10;
    await time.sleep(1);
    x = x + 5;
    printf("x = {}\n", x);
    await time.sleep(1);
    printf("x still = {}\n", x);
}

func test_bare_then_logic() {
    println("=== Bare then logic ===");
    do_bare_then_logic();
}

// --- Await: multiple bare ---

async func do_multiple_bare() Promise {
    await time.sleep(1);
    println("after first");
    await time.sleep(1);
    println("after second");
    await time.sleep(1);
    println("after third");
}

func test_multiple_bare() {
    println("=== Multiple bare ===");
    do_multiple_bare();
}

// --- timeout / sleep ---

func test_timeout() {
    println("=== timeout ===");
    time.timeout(10, func () {
        println("timeout fired");
    });
    println("scheduled");
}

func test_sleep() {
    println("=== sleep ===");
    time.sleep(10).then(func (u) {
        println("sleep resolved");
    });
    println("scheduled");
}

func test_sleep_capture() {
    println("=== sleep capture ===");
    var counter = 42;
    time.sleep(10).then(func [counter] (u) {
        printf("captured: {}\n", counter);
    });
    counter = 999;
    printf("mutated: {}\n", counter);
}

func main() {
    // Promise API
    test_manual_promise();
    test_promise_state();
    test_then_chain();
    test_promise_make();
    test_promise_string();
    test_promise_in_struct();

    // Async no await
    test_async_no_await();

    // Await patterns (sync-resolved)
    test_var_decl();
    test_await_in_func_arg();
    test_await_in_binop();
    test_multiple_awaits();
    test_locals_across();
    test_chain();
    test_nested_chain();
    test_return_await();
    test_await_comparison();
    test_interleaved();
    test_await_string();
    test_string_concat();
    test_await_last();
    test_double_chain();

    // Await patterns (async-resolved via event loop)
    test_bare_await();
    test_bare_then_logic();
    test_multiple_bare();

    // Timer
    test_timeout();
    test_sleep();
    test_sleep_capture();

    println("All tests passed!");
}

