// Delete in managed mode.
// expect-error: 'delete' is not allowed in managed mode

struct Foo {
    x: int = 0;
}

func main() {
    var f = new Foo{};
    delete f;
}
