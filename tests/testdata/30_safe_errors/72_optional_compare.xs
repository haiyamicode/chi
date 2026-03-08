// Comparing two optional values directly is not allowed — only null checks.
// expect-error: cannot compare optional values directly

func main() {
    var a: ?int = 42;
    var b: ?int = 10;
    if a == b {
        println("equal");
    }
}
