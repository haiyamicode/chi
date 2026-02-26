// expect-error: variadic pack expands to 0 arguments, but at least 1 are required
func forward_print<...T>(args: ...T) {
    printf(args...);
}

func main() {
    forward_print();
}
