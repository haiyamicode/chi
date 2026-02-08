// Test lambda function type inference

// Simple struct for testing lambda struct returns
struct Point {
    x: int;
    y: int;

    func new(x: int, y: int) {
        this.x = x;
        this.y = y;
    }
}

// Generic wrapper for return type inference testing
struct Wrapper<T> {
    value: T;

    func new(v: T) {
        this.value = v;
    }
}

// Generic function that creates a Wrapper - T is inferred from return type
func make_wrapper<T>(provider: func () T) Wrapper<T> {
    return {provider()};
}

// Helper function that takes a lambda with int param and int return
func apply_int(x: int, f: func (n: int) int) int {
    return f(x);
}

// Helper function with two parameters in lambda
func combine(a: int, b: int, f: func (x: int, y: int) int) int {
    return f(a, b);
}

// Helper function that takes a lambda with char param
func apply_char(c: char, f: func (ch: char) char) char {
    return f(c);
}

// Transform function with int
func transform_int(value: int, f: func (x: int) int) int {
    return f(value);
}

// Function that takes a predicate lambda (returning bool)
func filter_positive(x: int, pred: func (n: int) bool) int {
    if pred(x) {
        return x;
    }
    return 0;
}

// Lambda that returns void
func for_each(x: int, f: func (n: int)) {
    f(x);
}

func main() {
    // Test 1: Basic parameter type inference
    printf("Test 1: Basic inference\n");
    var result = apply_int(5, func (n) { return n * 2; });
    printf("apply_int(5, n => n * 2) = {}\n", result);

    // Test 2: Multiple parameter inference
    printf("\nTest 2: Multiple params\n");
    var sum = combine(10, 20, func (x, y) { return x + y; });
    printf("combine(10, 20, (x, y) => x + y) = {}\n", sum);

    // Test 3: With explicit return type (param inferred)
    printf("\nTest 3: Explicit return, inferred params\n");
    var doubled = apply_int(7, func (n) int { return n * 2; });
    printf("apply_int(7, n => n * 2) = {}\n", doubled);

    // Test 4: Different types
    printf("\nTest 4: Char type\n");
    var upper = apply_char('a', func (c) { return (c as int - 32) as char; });
    printf("apply_char('a', to_upper) = {}\n", upper);

    // Test 5: Transform function with inferred lambda
    printf("\nTest 5: Transform with inference\n");
    var squared = transform_int(6, func (x) { return x * x; });
    printf("transform_int(6, x => x * x) = {}\n", squared);

    // Test 6: Return type inference (returns bool)
    printf("\nTest 6: Bool return\n");
    var positive = filter_positive(42, func (n) { return n > 0; });
    printf("filter_positive(42, n => n > 0) = {}\n", positive);

    var negative = filter_positive(-5, func (n) { return n > 0; });
    printf("filter_positive(-5, n => n > 0) = {}\n", negative);

    // Test 7: Void return inference
    printf("\nTest 7: Void return\n");
    for_each(100, func (n) { printf("for_each got: {}\n", n); });

    // Test 8: Mixed - some explicit, some inferred
    printf("\nTest 8: Mixed explicit/inferred\n");
    var mixed = apply_int(3, func (n: int) { return n + 10; });
    printf("apply_int(3, n: int => n + 10) = {}\n", mixed);

    // Test 9: Arrow syntax - basic
    printf("\nTest 9: Arrow syntax\n");
    var arrow1 = apply_int(4, func (n) => n * 3);
    printf("apply_int(4, func (n) => n * 3) = {}\n", arrow1);

    // Test 10: Arrow syntax - multiple params
    printf("\nTest 10: Arrow multiple params\n");
    var arrow2 = combine(5, 7, func (x, y) => x * y);
    printf("combine(5, 7, func (x, y) => x * y) = {}\n", arrow2);

    // Test 11: Arrow syntax with explicit param types
    printf("\nTest 11: Arrow explicit params\n");
    var arrow3 = apply_int(8, func (n: int) => n + 100);
    printf("apply_int(8, func (n: int) => n + 100) = {}\n", arrow3);

    // Test 12: Arrow returning bool
    printf("\nTest 12: Arrow bool return\n");
    var arrow4 = filter_positive(10, func (n) => n > 5);
    printf("filter_positive(10, func (n) => n > 5) = {}\n", arrow4);

    // Test 13: Return type inference for generic functions
    printf("\nTest 13: Return type inference\n");
    var wrapper: Wrapper<int> = make_wrapper(func () { return 42; });
    printf("make_wrapper inferred T=int from return type: {}\n", wrapper.value);

    // Test 14: Return type inference with string type
    printf("\nTest 14: String type inference\n");
    var str_wrapper: Wrapper<string> = make_wrapper(func () { return "hello"; });
    printf("make_wrapper inferred T=string from return type: {}\n", str_wrapper.value);

    // Test 15: Lambda returning struct (tests sret codegen fix)
    printf("\nTest 15: Lambda returning struct\n");
    var point_fn: func () Point = func () { return {10, 20}; };
    var p = point_fn();
    printf("lambda returning struct: Point({}, {})\n", p.x, p.y);

    // Test 16: Generic function with struct return type inference
    printf("\nTest 16: Struct return type inference\n");
    var point_wrapper: Wrapper<Point> = make_wrapper(func () { return {30, 40}; });
    printf("make_wrapper inferred T=Point: Point({}, {})\n", point_wrapper.value.x, point_wrapper.value.y);

    // Test 17: Array.filter with lambda
    printf("\nTest 17: Array.filter\n");
    var nums: Array<int> = {1, 2, 3, 4, 5, 6};
    var evens = nums.filter(func (n) => n % 2 == 0).map(func num =>
}
