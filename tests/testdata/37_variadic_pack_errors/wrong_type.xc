// expect-error: pack expansion type mismatch at position 1: expected string, got int
func forward_print<...T>(args: ...T) {
    printf(args...);
}

func main() {
    forward_print(42);
}
