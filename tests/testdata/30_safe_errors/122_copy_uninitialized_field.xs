// Direct field assignment in ops.Copy missing a field.
// expect-error: has not been initialized in copy()

import "std/ops" as ops;

struct Pair {
    x: int;
    y: int;

    mut func new(x: int, y: int) {
        this.x = x;
        this.y = y;
    }

    impl ops.Copy {
        mut func copy(source: &This) {
            this.x = source.x;
        }
    }
}

func main() {}
