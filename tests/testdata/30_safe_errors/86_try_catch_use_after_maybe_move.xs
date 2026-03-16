// Use after maybe-move: moved in catch, use after try/catch
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

struct TestError {
    impl Error {
        func message() string {
            return "test";
        }
    }
}

func consume(h: Heavy) {}

func may_throw() {
    throw new TestError{};
}

func main() {
    var h = Heavy{value: 1};
    try may_throw() catch {
        consume(move h);
    };
    println(h.value); // error: h may have been moved
}
