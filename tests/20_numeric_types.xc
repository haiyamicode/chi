// Test comprehensive numeric type support including int and float arithmetic
// and type conversions

func test_basic_types() {
    var i32: int32 = 100;
    var i64: int64 = 1000000;
    var ui32: uint32 = 2000000;
    var ui64: uint64 = 3000000;
    var f32: float = 3.14;
    var f64: float64 = 3.141592653589793;
    
    printf("Basic types:\n");
    printf("int32: {}\n", i32);
    printf("int64: {}\n", i64);
    printf("uint32: {}\n", ui32);
    printf("uint64: {}\n", ui64);
    printf("float: {}\n", f32);
    printf("float64: {}\n", f64);
}

func test_arithmetic() {
    var a: int32 = 100;
    var b: int32 = 50;
    var c: float = 3.5;
    var d: float = 1.5;
    
    printf("\nArithmetic operations:\n");
    printf("int32: {} + {} = {}\n", a, b, a + b);
    printf("int32: {} - {} = {}\n", a, b, a - b);
    printf("int32: {} * {} = {}\n", a, b, a * b);
    printf("int32: {} / {} = {}\n", a, b, a / b);
    
    printf("float: {} + {} = {}\n", c, d, c + d);
    printf("float: {} - {} = {}\n", c, d, c - d);
    printf("float: {} * {} = {}\n", c, d, c * d);
    printf("float: {} / {} = {}\n", c, d, c / d);
}

func test_type_conversions() {
    var i: int32 = 42;
    var f: float = 3.14;
    
    printf("\nType conversions:\n");
    printf("int32 to float: {} -> {}\n", i, i as float);
    printf("float to int32: {} -> {}\n", f, f as int32);
    printf("int32 to int64: {} -> {}\n", i, i as int64);
    printf("float to float64: {} -> {}\n", f, f as float64);
}

func test_mixed_operations() {
    var i: int32 = 100;
    var f: float = 2.5;
    
    printf("\nMixed operations (with explicit casts):\n");
    printf("int32 + float: {} + {} = {}\n", i, f, (i as float) + f);
    printf("float - int32: {} - {} = {}\n", f, i, f - (i as float));
    printf("int32 * float: {} * {} = {}\n", i, f, (i as float) * f);
    printf("float / int32: {} / {} = {}\n", f, i, f / (i as float));
}

func test_unary_operations() {
    var i: int32 = 42;
    var f: float = 3.14;
    
    printf("\nUnary operations:\n");
    printf("-int32: {}\n", -i);
    printf("-float: {}\n", -f);
    printf("+int32: {}\n", +i);
    printf("+float: {}\n", +f);
}

func test_edge_cases() {
    var max_i32: int32 = 2147483647;
    var min_i32: int32 = -2147483648;
    var small_float: float = 0.0001;
    var large_float: float = 1000000.0;
    
    printf("\nEdge cases:\n");
    printf("max int32: {}\n", max_i32);
    printf("min int32: {}\n", min_i32);
    printf("small float: {}\n", small_float);
    printf("large float: {}\n", large_float);
}

func main() {
    test_basic_types();
    test_arithmetic();
    test_type_conversions();
    test_mixed_operations();
    test_unary_operations();
    test_edge_cases();
    printf("All numeric type tests completed!\n");
}