// Tuple enum variant payload carrying a local ref must not escape.
// expect-error: does not live long enough

struct Data {
    value: int = 0;
}

enum RefValue {
    Ref(&Data),
}

func make() RefValue {
    var x = Data{value: 1};
    return RefValue.Ref{&x};
}

func main() {}
