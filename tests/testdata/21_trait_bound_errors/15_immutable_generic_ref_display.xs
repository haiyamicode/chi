// Generic immutable-reference diagnostics should use display names, not internal ids
// expect-error: immutable reference Holder<T> cannot be modified
struct Holder<T> {
    value: T;
}

func bad<T>(h: &Holder<T>, v: T) {
    h.value = v;
}
