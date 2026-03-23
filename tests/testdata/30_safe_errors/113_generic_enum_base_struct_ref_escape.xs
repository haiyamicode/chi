// Generic enum base-struct field carrying a local ref must not escape.
// expect-error: does not live long enough

struct Data {
    value: int = 0;
}

enum Wrap<T> {
    A;

    struct {
        value: T;
    }
}

func make() Wrap<&Data> {
    var x = Data{value: 1};
    return Wrap<&Data>.A{value: &x};
}

func main() {}
