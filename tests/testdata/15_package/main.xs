import "stdio" as stdio;
import "std/mem" as mem;
import "math" as math;

// Test module that re-exports C functions
import {strlen, strcmp} from "./src/c_strings";

// Test inline extern declarations (traditional approach)
extern "C" {
    func lib_compute() int64;
    func util_func() int64;
}





// Test C header import with wildcard pattern
extern "C" {
    import {strcpy, mem*} from "string.h";
}




func main() {
    let lib_result = lib_compute();
    printf("number from library: {}\n", lib_result);

    let util_result = util_func();
    printf("util result: {}\n", util_result);

    let total = lib_result + util_result;
    printf("total: {}\n", total);

    // Math library demonstrations using imported functions
    let sqrt_result = math.sqrtf(16.0);
    printf("sqrt(16) = {}\n", sqrt_result);

    let sin_result = math.sinf(1.5708); // π/2
    printf("sin(π/2) = {}\n", sin_result);

    let cos_result = math.cosf(0.0);
    printf("cos(0) = {}\n", cos_result);

    let pow_result = math.powf(2.0, 8.0);
    printf("pow(2, 8) = {}\n", pow_result);

    // Test C variadic function (printf via stdio module)
    var msg1 = "Variadic test: single arg\n";
    stdio.printf(msg1.to_cstring().data);

    // Test printf with multiple integer arguments
    var fmt1 = "Variadic test: %d + %d = %d\n";
    stdio.printf(fmt1.to_cstring().data, 10, 20, 30);

    // Test printf with mixed argument types
    var fmt2 = "Variadic test: int=%d, float=%f\n";
    stdio.printf(fmt2.to_cstring().data, 42, 3.14);

    // Test C string literals with C functions imported from header
    let hello = c"Hello";
    let world = c"World";
    let hello2 = c"Hello";

    printf("strlen('Hello') = {}\n", strlen(hello));
    printf("strlen('World') = {}\n", strlen(world));
    printf("strcmp('Hello', 'World') = {}\n", strcmp(hello, world));
    printf("strcmp('Hello', 'Hello') = {}\n", strcmp(hello, hello2));

    // Test strcpy from imported header
    unsafe {
        var buf = mem.malloc(100) as *byte;
        strcpy(buf, hello);

        var copied = string.from_raw(buf, 5);
        printf("strcpy result: {}\n", copied);
    }
}

