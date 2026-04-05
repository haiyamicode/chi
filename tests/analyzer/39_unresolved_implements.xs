// Struct implementing unresolved/nonexistent interface (was crashing in is_interface)
struct Foo implements Bar {
    x: int;
}

struct Baz implements {
    y: int;
}

struct Qux implements NonExistent, AlsoMissing {
    z: int;
}
