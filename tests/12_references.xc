import "std/ops" as ops;

extern "C" {
    // C math functions
    func sqrt(x: float64) float64;
    func snprintf(buf: *char, size: uint64, fmt: *char, ...) int32;
}

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

struct OptionalHolder {
    value: ?int = null;
}

func test_optional_ref() {
    println("testing optional ref:");
    var holder: OptionalHolder = {};
    var ref = &mut holder;
    ref.value = 42;
    printf("ref.value={}\n", ref.value!);
    printf("holder.value={}\n", holder.value!);
}

func main() {
    let p: Pos = {.x = 1, .y = 2};
    printf("p: {}\n", p);
    var pp = &mut p;
    pp.reset();
    printf("p: {}\n", p);
    test_optional_ref();
}

