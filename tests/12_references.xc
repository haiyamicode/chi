import "std/ops" as ops;

struct Pos implements ops.Display {
  x: int = 0;
  y: int = 0;

  mut func display() string {
    return string.format("({}, {})", this.x, this.y);
  }

  mut func reset() {
    this.x = 0;
    this.y = 0;
  }
}

// Regression test: optional field assignment through reference
// Tests fix for compile_copy using destination type instead of source type
struct OptionalHolder {
  value: ?int = null;
}

func test_optional_ref() {
  println("testing optional ref:");
  var holder: OptionalHolder = {};
  var ref = &mut holder;

  // This was failing: assigning int to ?int through reference
  // compile_copy was using source type (int) instead of dest type (?int)
  ref.value = 42;
  printf("ref.value={}\n", ref.value!);

  // Verify the original was modified
  printf("holder.value={}\n", holder.value!);
}

func main() {
  let p: Pos = {
    .x = 1,
    .y = 2,
  };
  printf("p: {}\n", p);

  var pp = &mut p;
  pp.reset();
  printf("p: {}\n", p);

  test_optional_ref();
}