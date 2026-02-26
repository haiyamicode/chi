// expect-error: pack expansion type mismatch at position 1: expected int, got string
func takes_int(x: int) {}
func takes_string(x: string) {}

func conflicting<...T>(args: ...T) {
    takes_int(args...);
    takes_string(args...);
}

func main() {
    conflicting("hello");
}
