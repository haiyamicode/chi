// expect-error: does not live long enough

struct Boxed {
    id: int = 0;
}

func collect() Map<int, &Boxed> {
    var m: Map<int, &Boxed> = {};
    for i in 0..2 {
        var obj = Boxed{id: 900 + i};
        m.set(i, &obj);
    }
    return m;
}

func main() {
    let _ = collect();
}
