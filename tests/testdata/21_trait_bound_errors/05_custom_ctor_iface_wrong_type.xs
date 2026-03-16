// User-defined constructor interface with wrong param type — must be rejected
// expect-error: does not satisfy trait bound
interface IntConstruct {
    func new(x: int);
}

struct WrongParam {
    s: string;

    mut func new(s: string) {
        this.s = s;
    }
}

struct Holder<T: IntConstruct> {
    func make(v: int) T {
        return {v};
    }
}

func main() {
    var h = Holder<WrongParam>{};
}
