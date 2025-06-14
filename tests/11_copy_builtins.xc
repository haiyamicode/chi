func test_string_pass(str: string) {
  printf("passed string: {}\n");
}

func test_string() {
  // ensure that strings are properly copied and not double-freed
  var s = stringf("hello {}", "world");
  var t = s;
  println(t); 
  test_string_pass(s);
}

func test_array() {
  // ensure that arrays are properly copied
  var a: Array<int> = {};
  a.add(1);
  a.add(2);
  var b = a;
  a.clear();
  a.add(3);
  printf("a.size: {}\n", a.len);
  printf("b.size: {}\n", b.len);
  printf("a[0]: {}\n", a[0]);
  printf("b[0]: {}\n", b[0]);
}

func main() {
  test_string();
  test_array();
}