// Constructor interface requires params but struct has no constructor — must be rejected
// expect-error: does not satisfy trait bound
interface IntConstruct {
    func new(x: int);
}

struct NoCtor {
    x: int = 0;
}

struct Holder<T: IntConstruct> {
    func make(v: int) T {
        return T{v};
    }
}

func main() {
    var h = Holder<NoCtor>{};
}
