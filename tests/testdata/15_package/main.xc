import "stdio" as stdio;
import "math" as math;

extern "C" {
  func lib_compute() int64;
  func util_func() int64;
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
  stdio.printf(cx_string_to_c(&msg1));

  // Test printf with multiple integer arguments
  var fmt1 = "Variadic test: %d + %d = %d\n";
  stdio.printf(cx_string_to_c(&fmt1), 10, 20, 30);

  // Test printf with mixed argument types
  var fmt2 = "Variadic test: int=%d, float=%f\n";
  stdio.printf(cx_string_to_c(&fmt2), 42, 3.14);
}