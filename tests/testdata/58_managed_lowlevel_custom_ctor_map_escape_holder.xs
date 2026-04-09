export struct Boxed {
    id: int;
}

export struct Inner {
    refs: Map<int, &Boxed> = {};
}

export struct Holder {
    inner: Inner;

    mut func new() {
        this.inner = {};
    }
}
