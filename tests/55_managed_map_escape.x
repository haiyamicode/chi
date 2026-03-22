struct Boxed {
    id: int;
}

func collect() Map<int, &Boxed> {
    var m: Map<int, &Boxed> = {};
    for i in 0..3 {
        var obj = Boxed{id: 800 + i};
        m.set(i, &obj);
    }
    return m;
}

func main() {
    var m = collect();
    printf("map = [{}, {}, {}]\n", m[0].id, m[1].id, m[2].id);
}
