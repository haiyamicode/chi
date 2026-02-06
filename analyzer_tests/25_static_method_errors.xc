// Test malformed static method calls and declarations
struct Foo {
    static func bar() {
        // Missing return type inference
        return
    }

    // Invalid static method call syntax
    static func baz(
}

func main() {
    // Call static method on non-existent type
    var x = NonExistent.method();

    // Call non-static method as static
    var y = string.length;

    // Incomplete static call
    var z = Foo.

    // Static method with malformed arguments
    Foo.bar(,,,);
}
