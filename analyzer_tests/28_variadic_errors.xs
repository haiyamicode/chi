// Test malformed variadic functions and calls
func test_variadic(...args: int) {
    // Incomplete variadic iteration
    for arg in args {
        printf(
    }
}

// Multiple variadic params (invalid)
func invalid_multi(...a: int, ...b: string) {
}

// Variadic not last parameter
func wrong_order(...args: int, x: int) {
}

func main() {
    // printf with malformed format string and args
    printf("test %d %s",

    // stringf incomplete
    var s = stringf("{}",

    // Nested incomplete variadic
    printf(stringf("{}",

    // Empty variadic call with trailing comma
    test_variadic(1, 2, 3,,,);

    // Call with mismatched types
    printf(
}
