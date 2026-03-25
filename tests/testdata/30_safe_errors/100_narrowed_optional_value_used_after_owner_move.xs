// Narrowed optional-value alias must depend on the owner even for non-borrowing payloads.
// expect-error: used after move
func main() {
    var opt: ?string = "hello";
    if let value = opt {
        var moved = move opt;
        println(value);
        if moved {}
    }
}
