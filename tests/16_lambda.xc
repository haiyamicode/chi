import "std/ops" as ops;

struct TrackedBox implements ops.CopyFrom<TrackedBox> {
    id: int;

    func new(id_val: int) {
        this.id = id_val;
        printf("TrackedBox({}) constructed\n", this.id);
    }

    func delete() {
        printf("TrackedBox({}) destroyed\n", this.id);
    }

    func copy_from(from: &TrackedBox) {
        printf("TrackedBox({}) copied from TrackedBox({})\n", this.id, from.id);
        this.id = from.id;
    }
}

func test_basic_lambda() {
    println("testing basic lambda:");
    var simple_lambda = func () {
        println("Hello from lambda!");
    };
    simple_lambda();
    var add_lambda = func (a: int, b: int) int {
        return a + b;
    };
    var result = add_lambda(5, 3);
    printf("add_lambda(5, 3) = {}\n", result);
    println("");
}

func test_lambda_capture() {
    println("testing lambda with capture:");
    var x: int = 10;
    var y: int = 20;
    var capture_lambda = func () int {
        return x + y;
    };
    var captured_result = capture_lambda();
    printf("captured x + y = {}\n", captured_result);
    var z: int = 5;
    var modify_lambda = func () {
        z = z * 2;
    };
    printf("z before modification: {}\n", z);
    modify_lambda();
    printf("z after modification: {}\n", z);
    println("");
}

func test_nested_lambda_simple() {
    println("testing simple nested lambda:");
    var outer_value: int = 100;
    var create_inner = func () {
        var inner_lambda = func () int {
            return outer_value;
        };
        var nested_result = inner_lambda();
        printf("nested lambda result: {}\n", nested_result);
    };
    create_inner();
    println("");
}

func test_nested_lambda_complex() {
    println("testing complex nested lambda scenarios:");
    var a: int = 1;
    var b: int = 2;
    var c: int = 3;
    var level1 = func () {
        var d: int = 4;
        var level2 = func () int {
            var e: int = 5;
            var level3 = func () int {
                return a + b + c + d + e;
            };
            return level3();
        };
        var result = level2();
        printf("triple nested result: {}\n", result);
    };
    level1();
    var counter: int = 0;
    var incrementer = func () {
        counter = counter + 1;
        var nested_incrementer = func () {
            counter = counter + 10;
            var deeply_nested = func () {
                counter = counter + 100;
            };
            deeply_nested();
        };
        nested_incrementer();
    };
    printf("counter before: {}\n", counter);
    incrementer();
    printf("counter after nested increments: {}\n", counter);
    var shared_base: int = 100;
    var adder = func (value: int) int {
        return value + shared_base;
    };
    var multiplier = func (value: int) int {
        return value * shared_base;
    };
    printf("adder(7) = {}\n", adder(7));
    printf("multiplier(3) = {}\n", multiplier(3));
    var x: int = 10;
    var y: int = 20;
    var z: int = 30;
    var chain_start = func () int {
        var local1: int = x + 1;
        var chain_middle = func () int {
            var local2: int = y + local1;
            var chain_end = func () int {
                var local3: int = z + local2;
                return local3 + x;
            };
            return chain_end();
        };
        return chain_middle();
    };
    var chain_result = chain_start();
    printf("lambda chain result: {}\n", chain_result);
    println("");
}

func test_recursive_lambda_capture() {
    println("testing lambda capture edge cases:");
    var base_value: int = 1;
    var conditional_capture = func (n: int) int {
        if n > 0 {
            return n + base_value;
        }
        return base_value;
    };
    var conditional_result = conditional_capture(5);
    printf("conditional capture result: {}\n", conditional_result);
    var outer_multiplier: int = 2;
    var create_conditional = func () int {
        var inner_conditional = func (flag: bool) int {
            if flag {
                return outer_multiplier * 10;
            }
            return outer_multiplier;
        };
        return inner_conditional(true) + inner_conditional(false);
    };
    var conditional_nested_result = create_conditional();
    printf("conditional nested result: {}\n", conditional_nested_result);
    println("");
}

func test_lambda_array_capture() {
    println("testing lambda with array captures:");
    var value1: int = 1;
    var value2: int = 2;
    var value3: int = 3;
    var sum: int = 0;
    var process_array = func () {
        var local_multiplier: int = 10;
        var nested_processor1 = func () {
            var deep_calculator = func () int {
                return value1 * local_multiplier + sum;
            };
            sum = sum + deep_calculator();
        };
        var nested_processor2 = func () {
            var deep_calculator = func () int {
                return value2 * local_multiplier + sum;
            };
            sum = sum + deep_calculator();
        };
        var nested_processor3 = func () {
            var deep_calculator = func () int {
                return value3 * local_multiplier + sum;
            };
            sum = sum + deep_calculator();
        };
        nested_processor1();
        nested_processor2();
        nested_processor3();
    };
    printf("sum before processing: {}\n", sum);
    process_array();
    printf("sum after multi-variable processing: {}\n", sum);
    println("");
}

struct Calculator {
    multiplier: int = 1;

    func multiply(value: int) int {
        return value * this.multiplier;
    }
}

func add_for_lambda_test(a: int, b: int) int {
    return a + b;
}

func test_method_to_lambda() {
    println("testing method to lambda conversion:");
    var calc: Calculator = {.multiplier = 3};
    var direct_result = calc.multiply(5);
    printf("Direct method call: {}\n", direct_result);
    var method_lambda = calc.multiply;
    var lambda_result = method_lambda(5);
    printf("Method as lambda: {}\n", lambda_result);
    var abstracted: func (value: int) int = method_lambda;
    var abstracted_result = abstracted(7);
    printf("Abstracted lambda interface: {}\n", abstracted_result);
    var calc2: Calculator = {.multiplier = 10};
    var method_lambda2 = calc2.multiply;
    var result2 = method_lambda2(2);
    printf("Different instance lambda: {}\n", result2);
    println("");
}

func test_function_to_lambda() {
    println("testing function to lambda conversion:");
    var fn_lambda = add_for_lambda_test;
    var result1 = fn_lambda(10, 20);
    printf("Function as lambda: {}\n", result1);
    var abstracted: func (a: int, b: int) int = fn_lambda;
    var result2 = abstracted(15, 25);
    printf("Abstracted function lambda: {}\n", result2);
    println("");
}

func test_lambda_copy_semantics() {
    println("testing lambda copy semantics:");
    var x: int = 42;
    var f1 = func () int {
        return x;
    };
    var f2 = f1;
    printf("f1(): {}, f2(): {}\n", f1(), f2());
    f1 = func () int {
        return 0;
    };
    printf("After f1 reassignment - f1(): {}, f2(): {}\n", f1(), f2());
    var y: int = 99;
    var f3 = func () int {
        return y;
    };
    f3 = f3;
    printf("After self-assignment: {}\n", f3());
    var a: int = 10;
    var b: int = 20;
    var f4 = func () int {
        return a;
    };
    printf("First lambda: {}\n", f4());
    f4 = func () int {
        return b;
    };
    printf("After reassignment: {}\n", f4());
    println("");
}

func test_lambda_capture_lifecycle() {
    println("testing lambda capture lifecycle:");
    var box: TrackedBox = {42};
    var f1 = func () int {
        return box.id;
    };
    printf("f1() = {}\n", f1());
    var f2 = f1;
    printf("f2() = {}\n", f2());
    var f3 = f1;
    printf("f3() = {}\n", f3());
    var box2: TrackedBox = {99};
    f1 = func () int {
        return box2.id;
    };
    printf("f1() = {}, f2() = {}, f3() = {}\n", f1(), f2(), f3());
    println("");
}

func main() {
    test_basic_lambda();
    test_lambda_capture();
    test_nested_lambda_simple();
    test_nested_lambda_complex();
    test_recursive_lambda_capture();
    test_lambda_array_capture();
    test_method_to_lambda();
    test_function_to_lambda();
    test_lambda_copy_semantics();
    test_lambda_capture_lifecycle();
    println("All lambda tests completed!");
}

