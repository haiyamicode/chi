// A regular mut method call does not count as field initialization in copy().
// expect-error: has not been initialized in copy()

import "std/ops" as ops;

struct Container {
    items: Array<int>;
    count: int;

    mut func new() {
        this.items = {};
        this.count = 0;
    }

    impl ops.Copy {
        mut func copy(source: &This) {
            this.items.push(0);
        }
    }
}

func main() {}
