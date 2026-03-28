// expect-error: invalid operator '+' on type ?int
struct Container {
    value: ?int = null;
}

func no_leak(c: Container) int {
    let ok = c.value && c.value > 3;
    if ok {
        return 1;
    }
    return c.value + 1;
}

func main() {
    println(no_leak({}));
}
