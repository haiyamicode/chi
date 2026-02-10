// Test C interop via extern "C" blocks

extern "C" {
    func sqrt(x: float64) float64;
    func pow(x: float64, y: float64) float64;
    func abs(x: int32) int32;
    func snprintf(buf: *char, size: uint64, fmt: *char, ...) int32;
}

func test_sqrt() {
    println("testing C sqrt:");
    let result = sqrt(16.0);
    printf("sqrt(16.0) = {}\n", result);

    let result2 = sqrt(2.0);
    printf("sqrt(2.0) = {}\n", result2);
}

func test_pow() {
    println("testing C pow:");
    let result = pow(2.0, 8.0);
    printf("pow(2.0, 8.0) = {}\n", result);

    let result2 = pow(3.0, 3.0);
    printf("pow(3.0, 3.0) = {}\n", result2);
}

func test_abs() {
    println("testing C abs:");
    let result = abs(-42);
    printf("abs(-42) = {}\n", result);

    let result2 = abs(99);
    printf("abs(99) = {}\n", result2);
}

func test_snprintf() {
    println("testing C snprintf:");
    var buf = cx_malloc(100, null) as *char;
    let fmt = c"Hello, %s! Number: %d";
    let world = c"World";
    let count = snprintf(buf, 100, fmt, world, 42);
    printf("snprintf returned: {}\n", count);

    var result: string = "";
    cx_string_from_chars(buf as *void, count as uint32, &result);
    printf("buffer: {}\n", result);
}

func main() {
    test_sqrt();
    test_pow();
    test_abs();
    test_snprintf();
}

