// Enum base-struct field carrying a local ref must not escape.
// expect-error: does not live long enough

struct Data {
    value: int = 0;
}

enum RefValue {
    A;

    struct {
        value: &Data;
    }
}

func make() RefValue {
    var x = Data{value: 1};
    return RefValue.A{value: &x};
}

func main() {}
