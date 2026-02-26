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

// Struct with variadic type pack
struct Wrapper<...T> {
}

// Mixed: regular type param + variadic pack
func mixed<U, ...T>(prefix: U, args: ...T) {
    forward_print(prefix, args...);
}

func main() {
    // Struct variadic pack instantiation
    var w1 = Wrapper<int>{};
    var w2 = Wrapper<int, string>{};
    var w3 = Wrapper<int, string, bool>{};


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

    // Mixed type params (regular + pack)
    mixed<string, int, bool>("mixed: {} {}\n", 42, true);
}

