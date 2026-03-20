// expect-error: does not live long enough

struct EscapeProbeBox {
    id: int;
}

struct Holder<T> {
    value: T;

    mutex func set(value: T) {
        this.value = value;
    }
}

func collect() Holder<&EscapeProbeBox> {
    var holder = Holder<&EscapeProbeBox>{};
    for i in 0..2 {
        var obj = EscapeProbeBox{id: i + 1};
        holder.set(&obj);
    }
    return holder;
}

func main() {
    let holder = collect();
    printf("{}\n", holder.value.id);
}
