// expect-error: pointer arithmetic requires unsafe block
func main() {
    var x: int = 42;
    var p: *int = &x;
    var p2 = p + 1;
}
