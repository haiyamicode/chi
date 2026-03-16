// Pointer cast outside unsafe block.
// expect-error: pointer cast requires unsafe block

func main() {
    var x: int = 42;
    var p = &x as *int;
}
