// Generic call returning a struct that hides a reference must still track the borrow.
// expect-error: used after move

struct Obj {
    value: int = 0;
}

struct GenericHiddenRef<T> {
    value: &T;

    mut func new(v: &'this T) {
        this.value = v;
    }
}

func identity<'a, T: 'a>(value: T) T {
    return value;
}

func main() {
    var obj = Obj{value: 42};
    var wrapped = identity<GenericHiddenRef<Obj>>(GenericHiddenRef<Obj>{&obj});
    var moved = move obj;
    printf("{}\n", wrapped.value.value);
}
