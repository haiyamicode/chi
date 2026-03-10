// Move in one case, then move again after switch — second move sees it as already sunk
// expect-error: used after move
import "std/ops" as ops;

struct Heavy {
    value: int;
    mut func delete() {}
    impl ops.Copy {
        mut func copy(source: &This) { this.value = source.value; }
    }
}

func consume(h: Heavy) {}

func main() {
    var h = Heavy{value: 1};
    var x = 1;
    switch x {
        1 => consume(move h),
        else => {}
    }
    consume(move h); // error: may have been moved
}
