// expect-error: invalid operator '+' on type ?int
func no_leak(x: ?int) int {
    let ok = x && x > 3;
    if ok {
        return 1;
    }
    return x + 1;
}

func main() {
    println(no_leak(5));
}
