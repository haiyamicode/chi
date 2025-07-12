extern "C" {
  func lib_compute() int64;
  func util_func() int64;
  
  // Math library functions with correct C signatures
  func sqrtf(x: float) float;
  func sinf(x: float) float;
  func cosf(x: float) float;
  func powf(x: float, y: float) float;
}

func main() {
  let lib_result = lib_compute();
  printf("number from library: {}\n", lib_result);
  
  let util_result = util_func();
  printf("util result: {}\n", util_result);
  
  let total = lib_result + util_result;
  printf("total: {}\n", total);
  
  // Math library demonstrations
  let sqrt_result = sqrtf(16.0);
  printf("sqrt(16) = {}\n", sqrt_result);
  
  let sin_result = sinf(1.5708); // π/2
  printf("sin(π/2) = {}\n", sin_result);
  
  let cos_result = cosf(0.0);
  printf("cos(0) = {}\n", cos_result);
  
  let pow_result = powf(2.0, 8.0);
  printf("pow(2, 8) = {}\n", pow_result);
}