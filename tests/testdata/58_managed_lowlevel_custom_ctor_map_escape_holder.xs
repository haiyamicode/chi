export struct Boxed {
    id: int;
}

export struct Inner {
    refs: Map<int, &Boxed> = {};
}

export struct Holder {
    inner: Inner;

    mutex func new() {
        this.inner = {};
    }
}
