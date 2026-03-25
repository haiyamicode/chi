func greet(name: string, greeting: string = "Hello") {
    printf("{}, {}!\n", greeting, name);
}

func format_number(n: int, width: int = 0, fill: byte = ' ') int {
    printf("n={}, width={}, fill='{}'\n", n, width, fill);
    return n;
}

func add_offset(x: int, offset: int = 10 * 2) int {
    return x + offset;
}

func add_generic_offset<T>(x: T, offset: int = 3) int {
    return offset;
}

func infer_generic_offset<T>(x: T, offset: int = 4) int {
    return offset;
}

struct Greeter {
    prefix: string;

    mut func new(prefix: string = "Hey") {
        this.prefix = prefix;
    }

    func say(name: string, suffix: string = "!") {
        printf("{} {}{}\n", this.prefix, name, suffix);
    }
}

import "std/ops" as ops;

struct Configurable {
    x: int;
    y: int;

    mut func new(x: int = 10, y: int = 20) {
        this.x = x;
        this.y = y;
    }
}

struct GenericHolder<T: ops.Construct> {
    item: T = {};

    func get_item() T {
        return this.item;
    }
}

struct ScoreBox<T> {
    value: T;

    func score(x: int = 5) int {
        return x;
    }
}

struct DefaultPair<T: ops.Construct> {
    value: T;
    extra: int;

    mut func new(value: T = {}, extra: int = 4) {
        this.value = value;
        this.extra = extra;
    }
}

struct Util<T> {
    static func score(x: int = 6) int {
        return x;
    }
}

func optional_trailing(x: int, label: ?string) {
    if label {
        printf("x={}, label={}\n", x, label);
    } else {
        printf("x={}, label=none\n", x);
    }
}

func main() {
    greet("Alice");
    greet("Bob", "Hi");
    format_number(42);
    format_number(42, 5);
    format_number(42, 5, '0');
    printf("add_offset(5) = {}\n", add_offset(5));
    printf("add_offset(5, 100) = {}\n", add_offset(5, 100));
    printf("add_generic_offset<int>(5) = {}\n", add_generic_offset<int>(5));
    printf("add_generic_offset<int>(5, 8) = {}\n", add_generic_offset<int>(5, 8));
    printf("infer_generic_offset(5) = {}\n", infer_generic_offset(5));
    printf("infer_generic_offset(5, 9) = {}\n", infer_generic_offset(5, 9));
    var g1 = Greeter{};
    g1.say("World");
    g1.say("World", "?");
    var g2 = Greeter{"Hello"};
    g2.say("Chi");

    // Default args through generic wrapper
    printf("\n-- Generic default args --\n");
    var gh1 = GenericHolder<Greeter>{};
    gh1.get_item().say("Generic");
    var gh2 = GenericHolder<Configurable>{};
    printf("config: ({}, {})\n", gh2.get_item().x, gh2.get_item().y);
    // Explicit override still works
    var gh3 = GenericHolder<Configurable>{item: {x: 1, y: 2}};
    printf("explicit: ({}, {})\n", gh3.get_item().x, gh3.get_item().y);
    // Multi-default-arg struct constructed directly
    var c = Configurable{};
    printf("direct: ({}, {})\n", c.x, c.y);
    var c2 = Configurable{x: 5};
    printf("partial: ({}, {})\n", c2.x, c2.y);
    var sb = ScoreBox<int>{value: 1};
    printf("method default: {}\n", sb.score());
    printf("method explicit: {}\n", sb.score(9));
    printf("static default: {}\n", Util<int>.score());
    printf("static explicit: {}\n", Util<int>.score(10));
    var dp = DefaultPair<int>{};
    var dp2 = DefaultPair<int>{extra: 8};
    printf("generic ctor direct: ({}, {})\n", dp.value, dp.extra);
    printf("generic ctor partial: ({}, {})\n", dp2.value, dp2.extra);

    // Trailing ?T params auto-default to null
    println("\n-- Optional param defaults --");
    optional_trailing(1, "hi");
    optional_trailing(2);
}
