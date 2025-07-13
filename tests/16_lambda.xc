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

func test_nested_lambda_simple() {
  println("testing simple nested lambda:");
  
  var outer_value: int = 100;
  
  var create_inner = func () {
    var inner_lambda = func () int {
      return outer_value;
    };
    
    var nested_result = inner_lambda();
    printf("nested lambda result: {}\n", nested_result);
  };
  
  create_inner();
  println("");
}

func test_nested_lambda_complex() {
  println("testing complex nested lambda scenarios:");
  
  // Test 1: Triple nested lambda with multiple captures
  var a: int = 1;
  var b: int = 2;
  var c: int = 3;
  
  var level1 = func () {
    var d: int = 4;
    var level2 = func () int {
      var e: int = 5;
      var level3 = func () int {
        // This lambda captures from three different scopes
        return a + b + c + d + e;
      };
      return level3();
    };
    var result = level2();
    printf("triple nested result: {}\n", result);
  };
  
  level1();
  
  // Test 2: Nested lambda with intermediate variable modification
  var counter: int = 0;
  
  var incrementer = func () {
    counter = counter + 1;
    var nested_incrementer = func () {
      counter = counter + 10;
      var deeply_nested = func () {
        counter = counter + 100;
      };
      deeply_nested();
    };
    nested_incrementer();
  };
  
  printf("counter before: {}\n", counter);
  incrementer();
  printf("counter after nested increments: {}\n", counter);
  
  // Test 3: Multiple lambdas with shared captures
  var shared_base: int = 100;
  
  var adder = func (value: int) int {
    return value + shared_base;
  };
  
  var multiplier = func (value: int) int {
    return value * shared_base;
  };
  
  printf("adder(7) = {}\n", adder(7));
  printf("multiplier(3) = {}\n", multiplier(3));
  
  // Test 4: Chain of nested lambdas with mixed captures
  var x: int = 10;
  var y: int = 20;
  var z: int = 30;
  
  var chain_start = func () int {
    var local1: int = x + 1;  // 11
    var chain_middle = func () int {
      var local2: int = y + local1;  // 31
      var chain_end = func () int {
        var local3: int = z + local2;  // 61
        return local3 + x;  // 71
      };
      return chain_end();
    };
    return chain_middle();
  };
  
  var chain_result = chain_start();
  printf("lambda chain result: {}\n", chain_result);
  
  println("");
}

func test_recursive_lambda_capture() {
  println("testing lambda capture edge cases:");
  
  var base_value: int = 1;
  
  // Test lambda that captures and uses it in conditional
  var conditional_capture = func (n: int) int {
    if (n > 0) {
      return n + base_value;
    }
    return base_value;
  };
  
  var conditional_result = conditional_capture(5);
  printf("conditional capture result: {}\n", conditional_result);
  
  // Test nested lambda with conditional capture
  var outer_multiplier: int = 2;
  
  var create_conditional = func () int {
    var inner_conditional = func (flag: bool) int {
      if (flag) {
        return outer_multiplier * 10;
      }
      return outer_multiplier;
    };
    
    return inner_conditional(true) + inner_conditional(false);
  };
  
  var conditional_nested_result = create_conditional();
  printf("conditional nested result: {}\n", conditional_nested_result);
  
  println("");
}

func test_lambda_array_capture() {
  println("testing lambda with array captures:");
  
  // This test is more complex as it involves capturing variables
  // that might be used in different contexts
  var value1: int = 1;
  var value2: int = 2;
  var value3: int = 3;
  var sum: int = 0;
  
  var process_array = func () {
    var local_multiplier: int = 10;
    
    var nested_processor1 = func () {
      var deep_calculator = func () int {
        // Capture multiple variables from different scopes
        return value1 * local_multiplier + sum;
      };
      
      sum = sum + deep_calculator();
    };
    
    var nested_processor2 = func () {
      var deep_calculator = func () int {
        return value2 * local_multiplier + sum;
      };
      
      sum = sum + deep_calculator();
    };
    
    var nested_processor3 = func () {
      var deep_calculator = func () int {
        return value3 * local_multiplier + sum;
      };
      
      sum = sum + deep_calculator();
    };
    
    nested_processor1();
    nested_processor2();
    nested_processor3();
  };
  
  printf("sum before processing: {}\n", sum);
  process_array();
  printf("sum after multi-variable processing: {}\n", sum);
  
  println("");
}

func main() {
  test_basic_lambda();
  test_lambda_with_timeout();
  test_lambda_capture();
  test_nested_lambda_simple();
  test_nested_lambda_complex();
  test_recursive_lambda_capture();
  test_lambda_array_capture();
  
  println("All lambda tests completed!");
}