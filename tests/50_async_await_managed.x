// Test async/await in managed memory mode (.x)
// Exercises patterns that stress GC + async interaction

import "std/time" as time;

async func double_it(x: int) Promise<int> {
    return x * 2;
}

async func add_async(a: int, b: int) Promise<int> {
    return a + b;
}

async func greet(name: string) Promise<string> {
    return "hello " + name;
}

// --- Promise API ---

func test_promise_api() {
    println("=== Promise API ===");
    var p = Promise<int>{};
    p.then(func (val: int) {
        printf("cb: {}\n", val);
    });
    printf("before: {}\n", p.is_resolved());
    p.resolve(42);
    printf("after: {}\n", p.is_resolved());
    printf("value: {}\n", p.value()!);
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
    var p = Promise<string>.make(func (resolve) {
        resolve("hello world");
    });
    printf("value: '{}'\n", p.value()!);

    var prefix = "captured";
    var p2 = Promise<string>.make(func [prefix] (resolve) {
        resolve(prefix + " value");
    });
    printf("captured: '{}'\n", p2.value()!);
}

func test_multiple_promises() {
    println("=== Multiple promises ===");
    var a = Promise<int>{};
    var b = Promise<int>{};
    var c = Promise<int>{};
    a.resolve(1);
    b.resolve(2);
    c.resolve(3);
    printf("sum: {}\n", a.value()! + b.value()! + c.value()!);
}

// --- Async no await ---

async func simple_async(x: int) Promise<int> {
    return x + 10;
}

func test_async_no_await() {
    println("=== Async no await ===");
    printf("value: {}\n", simple_async(7).value()!);
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

// --- Await: func arg ---

async func do_func_arg() Promise<int> {
    var result = await double_it(7);
    printf("in func arg: {}\n", result);
    return result;
}

func test_func_arg() {
    println("=== Func arg ===");
    printf("value: {}\n", do_func_arg().value()!);
}

// --- Await: binop ---

async func do_binop() Promise<int> {
    var x = await double_it(3) + 100;
    return x;
}

func test_binop() {
    println("=== Binop ===");
    printf("value: {}\n", do_binop().value()!);
}

// --- Await: multiple sequential ---

async func do_multiple() Promise<int> {
    var a = await double_it(1);
    var b = await double_it(2);
    var c = await double_it(3);
    return a + b + c;
}

func test_multiple() {
    println("=== Multiple ===");
    printf("value: {}\n", do_multiple().value()!);
}

// --- Await: locals across boundaries ---

async func do_locals() Promise<int> {
    var before = 42;
    var x = await double_it(10);
    return before + x;
}

func test_locals() {
    println("=== Locals across ===");
    printf("value: {}\n", do_locals().value()!);
}

// --- Await: chain ---

async func do_chain(start: int) Promise<int> {
    var s1 = await double_it(start);
    var s2 = await double_it(s1);
    var s3 = await double_it(s2);
    return s3;
}

func test_chain() {
    println("=== Chain ===");
    printf("value: {}\n", do_chain(5).value()!);
}

// --- Await: nested ---

async func inner(x: int) Promise<int> {
    var a = await double_it(x);
    return a + 1;
}

async func do_nested() Promise<int> {
    return await inner(10);
}

func test_nested() {
    println("=== Nested ===");
    printf("value: {}\n", do_nested().value()!);
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

async func do_comparison() Promise<bool> {
    var big = await double_it(5) > 8;
    return big;
}

func test_comparison() {
    println("=== Comparison ===");
    printf("value: {}\n", do_comparison().value()!);
}

// --- Await: interleaved ---

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

async func do_string() Promise<string> {
    var msg = await greet("world");
    return msg;
}

func test_string() {
    println("=== String ===");
    printf("value: '{}'\n", do_string().value()!);
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

// --- Await: double chain ---

async func double_chain(x: int) Promise<int> {
    var s1 = await double_it(x);
    var s2 = await double_it(s1);
    return s2;
}

async func do_double_chain() Promise<int> {
    return await double_chain(3);
}

func test_double_chain() {
    println("=== Double chain ===");
    printf("value: {}\n", do_double_chain().value()!);
}

// --- Await: string across multiple awaits ---

async func build_greeting() Promise<string> {
    var hello = await greet("world");
    var exclaimed = hello + "!";
    var upper = await greet("chi");
    return exclaimed + " " + upper;
}

func test_string_across_awaits() {
    println("=== String across awaits ===");
    printf("value: '{}'\n", build_greeting().value()!);
}

// --- Await: multiple async calls from same caller ---

func test_multiple_callers() {
    println("=== Multiple callers ===");
    var a = do_var_decl();
    var b = do_binop();
    var c = do_return_await();
    printf("a={}, b={}, c={}\n", a.value()!, b.value()!, c.value()!);
}

// --- Await: deeply nested ---

async func depth3(x: int) Promise<int> {
    return await double_it(x);
}

async func depth2(x: int) Promise<int> {
    var v = await depth3(x);
    return v + 1;
}

async func depth1(x: int) Promise<int> {
    var v = await depth2(x);
    return v + 1;
}

func test_deep_nesting() {
    println("=== Deep nesting ===");
    printf("value: {}\n", depth1(5).value()!);
}

// --- Await: with two-arg async ---

async func do_add() Promise<int> {
    var a = await double_it(3);
    var b = await double_it(4);
    return await add_async(a, b);
}

func test_add_async() {
    println("=== Add async ===");
    printf("value: {}\n", do_add().value()!);
}

// --- Await: bare then logic (async via event loop) ---

async func do_bare_await() Promise {
    await time.sleep(1);
    println("bare ok");
}

async func do_bare_logic() Promise {
    var x = 10;
    await time.sleep(1);
    x = x + 5;
    printf("x = {}\n", x);
    await time.sleep(1);
    printf("x still = {}\n", x);
}

async func do_multiple_bare() Promise {
    await time.sleep(1);
    println("after first");
    await time.sleep(1);
    println("after second");
    await time.sleep(1);
    println("after third");
}

func test_bare_await() {
    println("=== Bare await ===");
    do_bare_await();
}

func test_bare_logic() {
    println("=== Bare logic ===");
    do_bare_logic();
}

func test_multiple_bare() {
    println("=== Multiple bare ===");
    do_multiple_bare();
}

// --- Timer ---

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

// --- Struct field init with heap allocation ---

struct Payload {
    value: int;
}

struct Wrapper {
    payload: &Payload = new Payload{value: 77};
    label: string;
}

struct Nested {
    inner: &Wrapper = new Wrapper{label: "default"};
}

func test_struct_field_init() {
    println("=== Struct field init ===");
    // Default field value with new
    var w = Wrapper{label: "test"};
    printf("default payload: {}\n", w.payload.value);

    // Override default field value
    var w2 = Wrapper{payload: new Payload{value: 42}, label: "custom"};
    printf("custom payload: {}\n", w2.payload.value);

    // Nested struct with default new
    var n = Nested{};
    printf("nested label: '{}'\n", n.inner.label);
    printf("nested payload: {}\n", n.inner.payload.value);
}

func main() {
    // Struct field init (managed allocation)
    test_struct_field_init();

    // Promise API
    test_promise_api();
    test_promise_make();
    test_promise_string();
    test_multiple_promises();

    // Async no await
    test_async_no_await();

    // Await patterns (sync-resolved)
    test_var_decl();
    test_func_arg();
    test_binop();
    test_multiple();
    test_locals();
    test_chain();
    test_nested();
    test_return_await();
    test_comparison();
    test_interleaved();
    test_string();
    test_string_concat();
    test_double_chain();
    test_string_across_awaits();
    test_multiple_callers();
    test_deep_nesting();
    test_add_async();

    // Await patterns (async via event loop)
    test_bare_await();
    test_bare_logic();
    test_multiple_bare();

    // Timer
    test_timeout();
    test_sleep();
    test_sleep_capture();

    println("All managed async tests passed!");
}

