// Static constructors are not allowed
// expect-error: static 'new' is not allowed; use 'func new' for constructors
struct Bad {
    static func new() {
    }
}

func main() {
    var x = Bad{};
}
