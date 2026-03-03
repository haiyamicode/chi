func greet(name: string, greeting: string = "Hello") {
    printf("{}, {}!\n", greeting, name);
}

func format_number(n: int, width: int = 0, fill: byte = ' ') int {
    printf("n={}, width={}, fill={}\n", n, width, fill);
    return n;
}

func add_offset(x: int, offset: int = 10 * 2) int {
    return x + offset;
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
    var gh3 = GenericHolder<Configurable>{item: Configurable{x: 1, y: 2}};
    printf("explicit: ({}, {})\n", gh3.get_item().x, gh3.get_item().y);
    // Multi-default-arg struct constructed directly
    var c = Configurable{};
    printf("direct: ({}, {})\n", c.x, c.y);
    var c2 = Configurable{x: 5};
    printf("partial: ({}, {})\n", c2.x, c2.y);

    // Trailing ?T params auto-default to null
    println("\n-- Optional param defaults --");
    optional_trailing(1, "hi");
    optional_trailing(2);
}

