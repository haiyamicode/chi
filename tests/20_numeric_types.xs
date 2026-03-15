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

    // Narrowing casts (explicit)
    var big: int64 = 100000;
    printf("int64 to int32: {} -> {}\n", big, big as int32);
    var f64: float64 = 2.718281828;
    printf("float64 to float: {} -> {}\n", f64, f64 as float);
}

func test_mixed_operations() {
    var i: int32 = 100;
    var f: float = 2.5;
    printf("\nMixed operations (implicit conversions):\n");
    printf("int32 + float: {} + {} = {}\n", i, f, i + f);
    printf("float - int32: {} - {} = {}\n", f, i, f - i);
    printf("int32 * float: {} * {} = {}\n", i, f, i * f);
    printf("float / int32: {} / {} = {}\n", f, i, f / i);
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

func test_implicit_conversions() {
    var b: bool = true;
    var c: byte = 'A';
    var i8: int8 = 127;
    var i16: int16 = 32767;
    var i32: int32 = 0;
    var i64: int64 = 0;
    var f32: float = 0.0;
    var f64: float64 = 0.0;
    printf("\nImplicit integer conversions:\n");
    i32 = b;
    printf("bool to int32: {} -> {}\n", true, i32);
    i32 = c;
    printf("byte to int32: '{}' -> {}\n", c, i32);
    i32 = i8;
    printf("int8 to int32: {} -> {}\n", i8, i32);
    i64 = i16;
    printf("int16 to int64: {} -> {}\n", i16, i64);
    f32 = i32;
    printf("int32 to float: {} -> {}\n", i32, f32);
    f64 = i64;
    printf("int64 to float64: {} -> {}\n", i64, f64);
    f32 = 3.14;
    f64 = f32;
    printf("float to float64: {} -> {}\n", f32, f64);
    var result_i32: int32 = i8 + i16;
    printf("int8 + int16 = int32: {} + {} = {}\n", i8, i16, result_i32);
    var result_f32: float = i32 + f32;
    printf("int32 + float = float: {} + {} = {}\n", i32, f32, result_f32);
    var result_f64: float64 = f32 + f64;
    printf("float + float64 = float64: {} + {} = {}\n", f32, f64, result_f64);
}

func test_float_comparisons() {
    var a: float = 3.5;
    var b: float = 1.5;
    var c: float = 3.5;
    printf("\nFloat comparisons:\n");
    printf("{} < {} = {}\n", a, b, a < b);
    printf("{} > {} = {}\n", a, b, a > b);
    printf("{} <= {} = {}\n", a, c, a <= c);
    printf("{} >= {} = {}\n", a, c, a >= c);
    printf("{} == {} = {}\n", a, c, a == c);
    printf("{} != {} = {}\n", a, b, a != b);
    printf("{} == {} = {}\n", a, b, a == b);
    printf("{} != {} = {}\n", a, c, a != c);
}

func test_float_modulo() {
    var a: float = 7.5;
    var b: float = 2.5;
    printf("\nFloat modulo:\n");
    printf("{} % {} = {}\n", a, b, a % b);
    var c: float = 10.3;
    var d: float = 3.0;
    printf("{} % {} = {}\n", c, d, c % d);
}

func main() {
    test_basic_types();
    test_arithmetic();
    test_type_conversions();
    test_mixed_operations();
    test_unary_operations();
    test_edge_cases();
    test_implicit_conversions();
    test_float_comparisons();
    test_float_modulo();
    printf("All numeric type tests completed!\n");
}
