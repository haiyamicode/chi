// Raw pointer type in managed mode.
// expect-error: raw pointer types are not allowed in managed mode

func main() {
    var x: int = 42;
    var p: *int = &x;
}
