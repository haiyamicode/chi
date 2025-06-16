import "std/ops" as ops;

struct Pos implements ops.Display {
  x: int = 0;
  y: int = 0;

  mut func display() string {
    return stringf("({}, {})", this.x, this.y);
  }

  mut func reset() {
    this.x = 0;
    this.y = 0;
  }
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
}