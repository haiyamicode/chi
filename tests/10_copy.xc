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

func f() Foo {
  var a: Foo = {"fa"};
  a.p! = 42;
  var b = a;
  b.id = "fb";
  println("f() done");
  return b;
}

func main() {
  var foo = f();
  printf("g: {}\n", foo.p!);
  println("done");
}