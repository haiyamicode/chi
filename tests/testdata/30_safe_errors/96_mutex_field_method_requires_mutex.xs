// expect-error: mutex method

struct Buf {
    mutex func grow() {}
}

struct Holder {
    buf: Buf = {};

    mut func bad() {
        this.buf.grow();
    }
}

func main() {}
