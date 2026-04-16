// this.field.copy() initializes that field, but another field is still missing.
// expect-error: has not been initialized in copy()

import "std/ops" as ops;

struct Inner {
    value: int;

    mut func new(v: int) {
        this.value = v;
    }

    impl ops.Copy {
        mut func copy(source: &This) {
            this.value = source.value;
        }
    }
}

struct Outer {
    data: Inner;
    tag: int;

    mut func new(v: int, t: int) {
        this.data = Inner{v};
        this.tag = t;
    }

    impl ops.Copy {
        mut func copy(source: &This) {
            this.data.copy(&source.data);
        }
    }
}

func main() {}
