// expect-error: does not live long enough

struct EscapeProbeBox {
    id: int;
}

struct Holder<T> {
    value: T;

    func get() T {
        let tmp = this.value;
        return tmp;
    }
}

func leak() &EscapeProbeBox {
    var obj = EscapeProbeBox{id: 1};
    var holder = Holder<&EscapeProbeBox>{value: &obj};
    return holder.get();
}

func main() {
    let _ = leak();
}
