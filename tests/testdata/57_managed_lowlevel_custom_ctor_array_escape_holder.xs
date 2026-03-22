export struct Boxed {
    id: int;
}

export struct Inner {
    refs: Array<&Boxed> = [];
}

export struct Holder {
    inner: Inner;

    mutex func new() {
        this.inner = Inner{};
    }
}
