func test_optional() {
  println("testing optional:");
  var t: ?int = null;
  if !t {
    println("t is null");
  }
  t! = 5;
  if t {
    println("has value");
    printf("t={}\n", t);
  }
  println("");
}

func test_array() {
  println("testing array:");
  var a: Array<int> = {};
  a.add(1);
  a.add(2);
  printf("a=[{}, {}]\n", a[0], a[1]);
  a[0] = 2;
  a[1] = 1;
  printf("a=[{}, {}]\n", a[0], a[1]);
  println("");
}

func test_map() {
  println("testing map:");
  var m: Map<string, int> = {};
  m["abc"] = 1;
  m["d"] = 2;
  printf("m[\"abc\"] = {}\n", m["abc"]);
  printf("m[\"d\"] = {}\n", m["d"]);
  printf("m[\"ef\"] = {}\n", m["ef"]);
  var it1 = m.find("abc");
  printf("m.find(\"abc\")!! = {}\n", it1!!);
  var it2 = m.find("invalid");
  printf("m.find(\"invalid\") = {}\n", it2);
}

func main() {
  test_optional();
  test_array();
  test_map();
}