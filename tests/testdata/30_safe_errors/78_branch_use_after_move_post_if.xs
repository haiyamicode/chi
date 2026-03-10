// Use after move: both branches move, then use after if
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
        consume(move h);
    } else {
        consume(move h);
    }
    println(h.value); // error: moved in both branches
}

