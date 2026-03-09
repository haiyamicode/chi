// Comparing enums with base struct fields is not allowed.
// expect-error: invalid operator '=='

enum Token {
    Ident,
    Number;

    struct {
        line: int = 0;
    }
}

func main() {
    var a = Token.Ident;
    var b = Token.Number;
    if a == b {
        println("equal");
    }
}

