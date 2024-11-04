func test_string_pass(str string) {
  printf("passed string: {}\n");
}

func test_string() {
  // ensure that strings are properly copied and not double-freed
  var s = stringf("hello {}", "world");
  var t = s;
  println(t);
  test_string_pass(s);
}

func main() {
  test_string();
}