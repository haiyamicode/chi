import "std/ops" as ops;

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
    timeout(10, func () {
        println("timeout callback executed");
    });
    println("timeout scheduled");
    println("");
}

func test_sleep() {
    println("=== Test 9: sleep() function ===");
    sleep(10).then(func (u: Unit) {
        println("sleep resolved");
    });
    println("sleep scheduled");
    println("");
}

func test_sleep_value_capture() {
    println("=== Test 10: By-value capture with sleep ===");
    var counter = 42;
    sleep(10).then(func [counter] (u: Unit) {
        printf("captured counter: {}\n", counter);
    });
    counter = 999;
    printf("original counter after mutate: {}\n", counter);
    println("");
}

func test_promise_helper() {
    println("=== Test 11: Promise.make() ===");
    var p = Promise<int>.make(func (resolve: func (value: int)) {
        println("executor called");
        resolve(123);
    });
    printf("promise resolved with: {}\n", p.value()!);
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
    println("All async/await tests passed!");
}

