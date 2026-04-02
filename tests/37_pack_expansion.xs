// Test pack expansion for homogeneous variadics

import "std/ops" as ops;

// Simple forwarding
func print_count(...values: any) {
    printf("count: {}\n", values.length);
}

func forward(...args: any) {
    print_count(...args);
}

// Multiple levels of forwarding
func level3(...args: any) {
    printf("level3: {}\n", args.length);
}

func level2(...args: any) {
    level3(...args);
}

func level1(...args: any) {
    level2(...args);
}

// Mix regular args with pack expansion
func print_all(...values: any) {
    for val, i in values {
        if i > 0 {
            printf(" ");
        }
        printf("{}", val);
    }
    printf("\n");
}

func prefix_and_forward(msg: string, ...args: any) {
    printf("{}: ", msg);
    print_all(...args);
}

// Empty pack expansion
func empty_forward(...args: any) {
    print_count(...args);
}

// Pack expansion with different types
func mixed_types(...values: any) {
    printf("mixed: {}\n", values.length);
}

func test_mixed(...args: any) {
    mixed_types(...args);
}

func forward_strings(...parts: string) {
    print_all(...parts);
}

func surround_strings(...parts: string) {
    print_all("<", ...parts, ">");
}

func inspect_any_span(...values: any) {
    printf("inspect any empty={} ptr_null={}\n", values.is_empty(), values.as_ptr() == null);
    if !values.is_empty() {
        printf("inspect any first={}\n", values[0]);
        printf("inspect any tail_len={}\n", values.slice(1, null).length);
    }
}

func inspect_string_span(...parts: string) {
    printf("inspect string empty={} ptr_null={}\n", parts.is_empty(), parts.as_ptr() == null);
    if !parts.is_empty() {
        printf("inspect string first={}\n", parts[0]);
        printf("inspect string tail={}\n", parts.slice(1, null));
        printf("inspect string head={}\n", parts.slice(null, 2));
    }
}

func inspect_forwarded_strings(...parts: string) {
    inspect_string_span(...parts);
}

func forward_shared_span(items: &[Shared<int>]) {
    print_count(...items);
}

struct TrackedAny {
    value: int = 0;

    mut func new(value: int) {
        this.value = value;
        printf("tracked.new({})\n", value);
    }

    mut func delete() {
        printf("tracked.delete({})\n", this.value);
    }

    impl ops.Copy {
        mut func copy(source: &This) {
            this.value = source.value;
            printf("tracked.copy({})\n", source.value);
        }
    }
}

func forward_tracked_any(...items: TrackedAny) {
    print_count(...items);
}

func main() {
    // Basic forwarding
    forward(1, 2, 3);

    // Multiple levels
    level1(10, 20, 30, 40);

    // With prefix
    prefix_and_forward("numbers", 100, 200, 300);

    // Empty pack
    empty_forward();

    // Different types
    test_mixed(42, "hello", true, 3.14);

    // Runtime-sized pack forwarding with element conversion
    forward_strings("red", "green", "blue");
    surround_strings("alpha", "beta");
    surround_strings();
    inspect_any_span();
    inspect_any_span(7, "x", true);
    inspect_string_span("red", "green", "blue");
    inspect_forwarded_strings("left", "right", "center");

    // Single element
    forward(999);

    // Runtime-sized pack forwarding with owning coercion should release temps properly
    var shared = Shared<int>.from_value(123);
    var shared_items: Array<Shared<int>> = [shared, shared];
    printf("shared refs before: {}\n", shared.ref_count());
    forward_shared_span(shared_items.span());
    printf("shared refs after: {}\n", shared.ref_count());

    // User-defined Copy/delete through ...T -> ...any should link and clean up correctly
    forward_tracked_any({1}, {2});
}
