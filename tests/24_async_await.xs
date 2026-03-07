import "std/ops" as ops;
import "std/time" as time;

func test_manual_promise() {
    println("=== Test 1: Manual Promise with callbacks ===");
    var p = Promise<int>{};
    p.then(func (val: int) {
        printf("Callback A: {}\n", val);
    });
    p.then(func (val: int) {
        printf("Callback B: {}\n", val);
    });
    println("Resolving promise...");
    p.resolve(100);
    p.then(func (val: int) {
        printf("Callback C (after resolve): {}\n", val);
    });
    println("");
}

async func simple_async(x: int) Promise<int> {
    printf("simple_async({})\n", x);
    return x + 10;
}

func test_async_no_await() {
    println("=== Test 2: Async function without awaits ===");
    var p = simple_async(7);
    printf("Got result: {}\n", p.value()!);
    println("");
}

async func get_value(x: int) Promise<int> {
    printf("get_value({})\n", x);
    return x * 2;
}

async func compute_chain(start: int) Promise<int> {
    println("compute_chain: starting");
    var step1 = await get_value(start);
    printf("  step1 = {}\n", step1);
    var step2 = await get_value(step1);
    printf("  step2 = {}\n", step2);
    var step3 = await get_value(step2);
    printf("  step3 = {}\n", step3);
    println("compute_chain: done");
    return step3;
}

func test_async_chain() {
    println("=== Test 3: Async/await chaining ===");
    var result_promise = compute_chain(5);
    printf("Got promise, is_resolved: {}\n", result_promise.is_resolved());
    result_promise.then(func (final_value: int) {
        printf("Final value: {}\n", final_value);
    });
    println("");
}

async func nested_async(x: int) Promise<int> {
    var inner = await get_value(x);
    return inner + 100;
}

func test_nested() {
    println("=== Test 4: Nested async calls ===");
    var p = nested_async(3);
    printf("Result: {}\n", p.value()!);
    println("");
}

async func delayed_compute(x: int) Promise<int> {
    return x * 3;
}

func test_promise_state() {
    println("=== Test 5: Promise state checking ===");
    var p = Promise<int>{};
    printf("Before resolve - is_resolved: {}\n", p.is_resolved());
    p.resolve(42);
    printf("After resolve - is_resolved: {}\n", p.is_resolved());
    printf("Value: {}\n", p.value()!);
    println("");
}

func test_multiple_callbacks() {
    println("=== Test 6: Multiple callbacks on same promise ===");
    var p = Promise<int>{};
    var count: int = 0;
    p.then(func (val: int) {
        printf("Callback 1: {}\n", val);
    });
    p.then(func (val: int) {
        printf("Callback 2: {}\n", val);
    });
    p.then(func (val: int) {
        printf("Callback 3: {}\n", val);
    });
    p.resolve(99);
    println("");
}

async func compute_with_locals(a: int, b: int) Promise<int> {
    println("compute_with_locals: starting");
    var local1 = a * 2;
    printf("  local1 = {}\n", local1);
    var awaited = await get_value(b);
    printf("  awaited = {}\n", awaited);
    var result = local1 + awaited;
    printf("  result = {}\n", result);
    return result;
}

func test_locals_across_await() {
    println("=== Test 7: Local variables across await ===");
    var p = compute_with_locals(5, 10);
    printf("Final: {}\n", p.value()!);
    println("");
}

func test_timeout() {
    println("=== Test 8: timeout() function ===");
    var called = false;
    time.timeout(10, func () {
        println("timeout callback executed");
    });
    println("timeout scheduled");
    println("");
}

func test_sleep() {
    println("=== Test 9: sleep() function ===");
    time.sleep(10).then(func (u) {
        println("sleep resolved");
    });
    println("sleep scheduled");
    println("");
}

func test_sleep_value_capture() {
    println("=== Test 10: By-value capture with sleep ===");
    var counter = 42;
    time.sleep(10).then(func [counter] (u) {
        printf("captured counter: {}\n", counter);
    });
    counter = 999;
    printf("original counter after mutate: {}\n", counter);
    println("");
}

func test_promise_helper() {
    println("=== Test 11: Promise.make() ===");
    var p = Promise<int>.make(func (resolve) {
        println("executor called");
        resolve(123);
    });
    printf("promise resolved with: {}\n", p.value()!);
    println("");
}

func test_promise_string_make() {
    println("=== Test 12: Promise<string>.make() ===");
    // Sync resolve through make
    var p1 = Promise<string>.make(func (resolve) {
        resolve("hello world");
    });
    printf("sync make: '{}'\n", p1.value()!);

    // Deferred resolve through make
    var p2 = Promise<string>.make(func (resolve) {
        resolve("deferred string");
    });
    p2.then(func (v: string) {
        printf("then callback: '{}'\n", v);
    });

    // Make with string concatenation
    var prefix = "hello";
    var p3 = Promise<string>.make(
        func [prefix] (resolve) {
            resolve(prefix + " from capture");
        }
    );
    printf("captured: '{}'\n", p3.value()!);

    // Multiple .then callbacks with string
    var p4 = Promise<string>.make(func (resolve) {
        resolve("multi");
    });
    p4.then(func (v: string) {
        printf("cb1: '{}'\n", v);
    });
    p4.then(func (v: string) {
        printf("cb2: '{}'\n", v);
    });

    // Deferred string promise - register then before resolve
    var p5 = Promise<string>{};
    p5.then(func (v: string) {
        printf("deferred then: '{}'\n", v);
    });
    p5.resolve("resolved later");

    println("");
}

struct Container {
    static func make_promise(msg: string) Promise<string> {
        return Promise<string>.make(func [msg] (resolve) {
            resolve(msg);
        });
    }
}

func test_promise_string_in_struct() {
    println("=== Test 13: Promise<string> in static method ===");
    var p = Container.make_promise("from struct");
    printf("struct make: '{}'\n", p.value()!);
    println("");
}

func main() {
    test_manual_promise();
    test_async_no_await();
    test_async_chain();
    test_nested();
    test_promise_state();
    test_multiple_callbacks();
    test_locals_across_await();
    test_timeout();
    test_sleep();
    test_sleep_value_capture();
    test_promise_helper();
    test_promise_string_make();
    test_promise_string_in_struct();
    println("All async/await tests passed!");
}

