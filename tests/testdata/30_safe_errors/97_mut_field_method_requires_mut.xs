// expect-error: mutating method

struct Buf {
    mut func grow() {}
}

struct Holder {
    buf: Buf = {};

    func bad() {
        this.buf.grow();
    }
}

func main() {}
