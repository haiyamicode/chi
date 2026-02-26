// expect-error: pack expansion type mismatch at position 1: expected string, got int
func forward_print<...T>(args: ...T) {
    printf(args...);
}

func relay<...T>(args: ...T) {
    forward_print(args...);
}

func main() {
    relay(42);
}
