// Indirect call returning a generic struct that hides a reference must still track the borrow.
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

func pass_hidden(value: GenericHiddenRef<Obj>) GenericHiddenRef<Obj> {
    return value;
}

func main() {
    var f = pass_hidden;
    var obj = Obj{value: 42};
    var wrapped = f(GenericHiddenRef<Obj>{&obj});
    var moved = move obj;
    printf("{}\n", wrapped.value.value);
}
