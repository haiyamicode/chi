// Use after move in nested branches
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
    if true {
        if true {
            consume(move h);
        }
    }
    println(h.value); // error: h may have been moved (nested)
}
