// Dangling ref inside guard clause: ref borrows t, t moved in then-branch
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
    var ref = &h;
    if true {
        consume(move h); // h moved, ref dangles
        println(ref.value); // error: ref's source moved
        return;
    }
}
