import "std/ops" as ops;

struct Color implements ops.Display {
  r: int = 0;
  g: int = 0;
  b: int = 0;

  func new(r: int, g: int, b: int) {
    this.r = r;
    this.g = g;
    this.b = b;
  }

  static func black() Color {
    return {0, 0, 0};
  }

  static func white() Color {
    return {255, 255, 255};
  }

  static func gray(f: float) Color {
    return this.white().multiply(f);
  }

  static func red() Color {
    return {255, 0, 0};
  }

  func multiply(f: float) Color {
    let rf = this.r as float;
    let gf = this.g as float;
    let bf = this.b as float;
    let r = (rf * f) as int;
    let g = (gf * f) as int;
    let b = (bf * f) as int;
    return {r, g, b};
  }

  func display() string {
    return stringf("rgb({},{},{})", this.r, this.g, this.b);
  }

  func brightness() float {
    let t = this.r + this.g + this.b;
    return (t as float) / 3.0 / 255.0;
  }
}

func main() {
  let a = Color.black();
  printf("a: {}\n", a);

  let b = Color.gray(0.6);
  printf("b: {}\n", b);

  let c = Color.white();
  printf("c: {}\n", c);

  let d = Color.red();
  printf("d: {}\n", d);

  printf("a.brightness = {}\n", a.brightness());
  printf("b.brightness = {}\n", b.brightness());
  printf("c.brightness = {}\n", c.brightness());
  printf("d.brightness = {}\n", d.brightness());
}
