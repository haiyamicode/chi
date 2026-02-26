// expect-error: cannot convert from string to int
func forward_print<...T>(args: ...T) {
    printf(args...);
}

func main() {
    forward_print<string, int>("value: {}\n", "not_an_int");
}
