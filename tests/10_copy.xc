import "std/ops" as ops;

struct Foo implements ops.CopyFrom<Foo> {
  p: *int = null;
  id: string;

  mut func new(id: string) {
    this.id = id;
    this.p = cx_malloc(sizeof int, null);
    cx_memset(this.p, 0, sizeof int);
    printf("creating {}\n", this.id);
  }

  mut func copy_from(b: &Foo) {
    this.new(stringf("{}_copy", b.id));
    this.p! = b.p!;
    printf("copied {}, p = {}\n", this.id, b.p!);
  }

  func delete() {
    printf("deleting {}\n", this.id);
    cx_free(this.p);
  }
}

// Test 1: Return local variable (one copy expected)
func return_local() Foo {
  var a: Foo = {"local"};
  a.p! = 42;
  var b = a;
  b.id = "local_b";
  println("return_local() done");
  return b;
}

// Test 2: RVO - return ConstructExpr directly (zero copies expected)
func return_construct() Foo {
  println("return_construct() returning ConstructExpr");
  return {"direct"};
}

func main() {
  println("=== Test 1: Return local variable ===");
  var foo = return_local();
  printf("result: {}\n", foo.p!);

  println("=== Test 2: RVO - return ConstructExpr ===");
  var bar = return_construct();
  bar.p! = 99;
  printf("result: {}\n", bar.p!);

  println("done");
}