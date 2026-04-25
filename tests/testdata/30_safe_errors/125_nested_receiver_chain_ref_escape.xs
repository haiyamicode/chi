// Nested receiver chain returning &T must not escape the receiver's lifetime.
// expect-error: does not live long enough

struct NestedRejInner {
    val: uint32 = 0;

    func read() &uint32 {
        return &this.val;
    }
}

struct NestedRejMid {
    inner: NestedRejInner = {};
}

struct NestedRejOuter {
    mid: NestedRejMid = {};

    func observe() &uint32 {
        return this.mid.inner.read();
    }
}

func bad() &uint32 {
    var o = NestedRejOuter{};
    return o.observe();
}

func main() {}
