export struct Boxed {
    id: int;
}

export struct Holder {
    refs: Array<&Boxed> = [];
}
