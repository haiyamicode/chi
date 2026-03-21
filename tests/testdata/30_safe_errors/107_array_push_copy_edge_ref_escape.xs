// expect-error: does not live long enough

struct EscapeProbeBox {
    id: int;
}

func collect() Array<&EscapeProbeBox> {
    var refs: Array<&EscapeProbeBox> = [];
    for i in 0..2 {
        var obj = EscapeProbeBox{id: i + 1};
        refs.push(&obj);
    }
    return refs;
}

func main() {
    let refs = collect();
    printf("{}\n", refs[0].id);
}
