// expect-error: does not live long enough

struct EscapeProbeBox {
    id: int;
}

struct Holder<T> {
    value: T;

    mut func new(value: T) {
        let tmp = value;
        this.value = tmp;
    }
}

func collect() Holder<&EscapeProbeBox> {
    var obj = EscapeProbeBox{id: 1};
    return Holder<&EscapeProbeBox>{&obj};
}

func main() {
    let holder = collect();
    printf("{}\n", holder.value.id);
}
