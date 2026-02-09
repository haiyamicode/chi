struct Point {
    x: int;
    y: int;

    func new(x: int, y: int) {
        this.x = x;
        this.y = y;
    }
}

struct Wrapper<T> {
    value: T;

    func new(v: T) {
        this.value = v;
    }
}

func make_wrapper<T>(provider: func () T) Wrapper<T> {
    return {provider()};
}

func apply_int(x: int, f: func (n: int) int) int {
    return f(x);
}

func combine(a: int, b: int, f: func (x: int, y: int) int) int {
    return f(a, b);
}

func apply_char(c: char, f: func (ch: char) char) char {
    return f(c);
}

func transform_int(value: int, f: func (x: int) int) int {
    return f(value);
}

func filter_positive(x: int, pred: func (n: int) bool) int {
    if pred(x) {
        return x;
    }
    return 0;
}

func for_each(x: int, f: func (n: int)) {
    f(x);
}

func main() {
    printf("Test 1: Basic inference\n");
    var result = apply_int(5, func (n) {
        return n * 2;
    });
    printf("apply_int(5, n => n * 2) = {}\n", result);

    printf("\nTest 2: Multiple params\n");
    var sum = combine(10, 20, func (x, y) {
        return x + y;
    });
    printf("combine(10, 20, (x, y) => x + y) = {}\n", sum);
    printf("\nTest 3: Explicit return, inferred params\n");
    var doubled = apply_int(7, func (n) int {
        return n * 2;
    });
    printf("apply_int(7, n => n * 2) = {}\n", doubled);
    printf("\nTest 4: Char type\n");
    var upper = apply_char('a', func (c) {
        return (c as int - 32) as char;
    });
    printf("apply_char('a', to_upper) = {}\n", upper);
    printf("\nTest 5: Transform with inference\n");
    var squared = transform_int(6, func (x) {
        return x * x;
    });
    printf("transform_int(6, x => x * x) = {}\n", squared);
    printf("\nTest 6: Bool return\n");
    var positive = filter_positive(42, func (n) {
        return n > 0;
    });
    printf("filter_positive(42, n => n > 0) = {}\n", positive);
    var negative = filter_positive(-5, func (n) {
        return n > 0;
    });
    printf("filter_positive(-5, n => n > 0) = {}\n", negative);
    printf("\nTest 7: Void return\n");
    for_each(100, func (n) {
        printf("for_each got: {}\n", n);
    });
    printf("\nTest 8: Mixed explicit/inferred\n");
    var mixed = apply_int(3, func (n: int) {
        return n + 10;
    });
    printf("apply_int(3, n: int => n + 10) = {}\n", mixed);
    printf("\nTest 9: Arrow syntax\n");
    var arrow1 = apply_int(4, (n) => n * 3);
    printf("apply_int(4, func (n) => n * 3) = {}\n", arrow1);
    printf("\nTest 10: Arrow multiple params\n");
    var arrow2 = combine(5, 7, (x, y) => x * y);
    printf("combine(5, 7, func (x, y) => x * y) = {}\n", arrow2);
    printf("\nTest 11: Arrow explicit params\n");
    var arrow3 = apply_int(8, (n: int) => n + 100);
    printf("apply_int(8, func (n: int) => n + 100) = {}\n", arrow3);
    printf("\nTest 12: Arrow bool return\n");
    var arrow4 = filter_positive(10, (n) => n > 5);
    printf("filter_positive(10, func (n) => n > 5) = {}\n", arrow4);
    printf("\nTest 13: Return type inference\n");
    var wrapper: Wrapper<int> = make_wrapper(func () {
        return 42;
    });
    printf("make_wrapper inferred T=int from return type: {}\n", wrapper.value);
    printf("\nTest 14: String type inference\n");
    var str_wrapper: Wrapper<string> = make_wrapper(func () {
        return "hello";
    });
    printf("make_wrapper inferred T=string from return type: {}\n", str_wrapper.value);
    printf("\nTest 15: Lambda returning struct\n");
    var point_fn: func () Point = func () {
        return {10, 20};
    };
    var p = point_fn();
    printf("lambda returning struct: Point({}, {})\n", p.x, p.y);
    printf("\nTest 16: Struct return type inference\n");
    var point_wrapper: Wrapper<Point> = make_wrapper(func () {
        return {30, 40};
    });
    printf("make_wrapper inferred T=Point: Point({}, {})\n", point_wrapper.value.x, point_wrapper.value.y);
    printf("\nTest 17: Array.filter\n");
    var nums: Array<int> = {1, 2, 3, 4, 5, 6};
    var evens = nums.filter((n) => n % 2 == 0);
    printf("filter evens from [1,2,3,4,5,6]: ");

    for n in evens {
        printf("{} ", n);
    }

    printf("\n");
    printf("\nTest 18: Array.map\n");
    var doubled_arr = nums.map<int>(func (n) {
        return n * 2;
    });
    printf("map *2 on [1,2,3,4,5,6]: ");

    for n in doubled_arr {
        printf("{} ", n);
    }

    printf("\n");
    printf("\nTest 19: Array.map returning struct\n");
    var points = nums.map<Point>(func (n) {
        return Point{n, n * 10};
    });
    printf("map to Points: ");

    for p in points {
        printf("({},{}) ", p.x, p.y);
    }

    printf("\n");
    printf("\nAll lambda inference tests passed!\n");
}

