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
  
  // Alternative construct syntax
  var b = Array<int>{10, 20, 30};
  printf("b=[{}, {}, {}]\n", b[0], b[1], b[2]);
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
  println("");
}

func test_shared() {
  println("testing shared:");
  var r1: Shared<int> = {42};
  printf("r1.as_ref()={}, ref_count={}\n", r1.as_ref()!, r1.ref_count());

  var r2: Shared<int> = r1;
  printf("after copy: ref_count={}\n", r1.ref_count());

  r1.set(100);
  printf("after r1.set(100): r2.as_ref()={}\n", r2.as_ref()!);
}

// Regression test: nested {{}} construction with Shared
// Tests fix for 'new' allocation size using element type instead of pointer type
struct NestedState {
  value: int = 0;
  count: int = 0;
}

struct NestedShared {
  data: Shared<NestedState>;

  func new() {
    // This {{}} pattern was allocating wrong size (pointer size instead of element size)
    this.data = {{}};
  }

  func get_count() int {
    return this.data.as_ref().count;
  }

  func ref_count() uint32 {
    return this.data.ref_count();
  }
}

func test_nested_shared() {
  println("testing nested shared:");
  var ns: NestedShared = {};
  printf("ns.ref_count()={}\n", ns.ref_count());
  printf("ns.get_count()={}\n", ns.get_count());
}

func main() {
  test_optional();
  test_array();
  test_map();
  test_shared();
  test_nested_shared();
}