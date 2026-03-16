// Use after maybe-move: moved in one switch case only, use after switch
// expect-error: used after move
import "std/ops" as ops;

struct Heavy {
    value: int;

    mut func delete() {}

    impl ops.Copy {
        mut func copy(source: &This) {
            this.value = source.value;
        }
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
    println(h.value); // error: h may have been moved
}
