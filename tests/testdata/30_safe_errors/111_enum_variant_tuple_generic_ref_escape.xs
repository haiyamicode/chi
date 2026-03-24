// Generic tuple enum variant payload carrying a local ref must not escape.
// expect-error: does not live long enough

struct Data {
    value: int = 0;
}

enum Wrap<T> {
    Item(T)
}

func make() Wrap<&Data> {
    var x = Data{value: 1};
    return Wrap<&Data>.Item{&x};
}

func main() {}
