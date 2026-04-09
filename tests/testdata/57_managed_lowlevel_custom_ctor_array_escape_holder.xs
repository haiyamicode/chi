export struct Boxed {
    id: int;
}

export struct Inner {
    refs: Array<&Boxed> = [];
}

export struct Holder {
    inner: Inner;

    mut func new() {
        this.inner = {};
    }
}
