// Empty construction of struct with required fields
// expect-error: missing field
struct Pair {
    a: int;
    b: string;
}

func main() {
    var p = Pair{};
}
