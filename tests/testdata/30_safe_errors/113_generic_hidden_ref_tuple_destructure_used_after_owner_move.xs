// Tuple destructure of a generic result that hides a reference must preserve borrow tracking.
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

func make_pair<'a, T: 'a>(value: T) Tuple<T, int> {
    return (value, 0);
}

func main() {
    var obj = Obj{value: 42};
    var (wrapped, _) = make_pair<GenericHiddenRef<Obj>>(GenericHiddenRef<Obj>{&obj});
    var moved = move obj;
    printf("{}\n", wrapped.value.value);
}
