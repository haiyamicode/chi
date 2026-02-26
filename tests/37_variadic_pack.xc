// Basic: forward args to printf
func forward_print<...T>(args: ...T) {
    printf(args...);
}

// Chaining: relay through another pack function
func relay<...T>(args: ...T) {
    forward_print(args...);
}

// Multiple args of different types
func multi_forward<...T>(args: ...T) {
    printf(args...);
}

// Unused pack param (no constraints — anything goes)
func unused_pack<...T>(args: ...T) {
}

// Homogeneous variadic expansion
func print_count(...args: any) {
    printf("homogeneous count: {}\n", args.length);
}

func forward_homogeneous(...args: any) {
    print_count(args...);
}

func main() {
    // Direct forwarding
    forward_print("value: {}\n", 42);
    forward_print("{} + {} = {}\n", 1, 2, 3);
    forward_print("hello {}\n", "world");

    // Chained forwarding
    relay("relayed: {}\n", 99);
    relay("{} and {}\n", "foo", "bar");

    // Unused pack — just verifying it compiles
    unused_pack(1);
    unused_pack("hello", 42, true);

    // Explicit type args (non-inference)
    forward_print<string, int>("explicit: {}\n", 100);
    forward_print<string, string, string>("{} {} {}\n", "x", "y");

    // Homogeneous variadic forwarding
    forward_homogeneous(1, 2, 3);
}

