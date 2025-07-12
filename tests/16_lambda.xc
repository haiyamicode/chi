func test_basic_lambda() {
  println("testing basic lambda:");
  
  // Test lambda with no parameters
  var simple_lambda = func () {
    println("Hello from lambda!");
  };
  simple_lambda();
  
  // Test lambda with parameters
  var add_lambda = func (a: int, b: int) int {
    return a + b;
  };
  var result = add_lambda(5, 3);
  printf("add_lambda(5, 3) = {}\n", result);
  
  println("");
}

func test_lambda_with_timeout() {
  println("testing lambda with timeout:");
  
  var counter: int = 0;
  
  // Test lambda passed to timeout function
  timeout(100, func () {
    counter = 42;
    println("Timeout callback executed!");
  });
  
  printf("Counter after timeout: {}\n", counter);
  
  println("");
}

func test_lambda_capture() {
  println("testing lambda with capture:");
  
  var x: int = 10;
  var y: int = 20;
  
  // Test lambda that captures variables from outer scope
  var capture_lambda = func () int {
    return x + y;
  };
  
  var captured_result = capture_lambda();
  printf("captured x + y = {}\n", captured_result);
  
  // Test modifying captured variables
  var z: int = 5;
  var modify_lambda = func () {
    z = z * 2;
  };
  
  printf("z before modification: {}\n", z);
  modify_lambda();
  printf("z after modification: {}\n", z);
  
  println("");
}

func main() {
  test_basic_lambda();
  test_lambda_with_timeout();
  test_lambda_capture();
  
  println("All lambda tests completed!");
}