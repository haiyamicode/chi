// Ordering structs without Ord implementation is not allowed.
// expect-error: invalid operator '<'

struct Foo {
    x: int;
}

func main() {
    var a = Foo{x: 1};
    var b = Foo{x: 2};
    if a < b {
        println("less");
    }
}
