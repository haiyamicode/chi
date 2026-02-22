// Return a method call that produces a dangling reference.
// The receiver dies when the function returns.
// expect-error: does not live long enough

struct Container {
    value: int = 0;

    func get_ref() &int {
        return &this.value;
    }
}

func get_dangling() &int {
    var c = Container { value: 99 };
    return c.get_ref();
}

func main() {
    printf("{}\n", get_dangling()!);
}
