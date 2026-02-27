// Test pack expansion for homogeneous variadics

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

    // Single element
    forward(999);
}

