struct Boxed {
    id: int;
}

func collect() Array<&Boxed> {
    var refs: Array<&Boxed> = [];
    for i in 0..3 {
        var obj = Boxed{id: 700 + i};
        refs.push(&obj);
    }
    return refs;
}

func main() {
    var refs = collect();
    printf("refs = [{}, {}, {}]\n", refs[0].id, refs[1].id, refs[2].id);
}
