// Narrowed optional-reference alias must borrow the optional owner for invalidation.
// expect-error: used after move
func main() {
    var local = 42;
    var opt: ?(&int) = &local;
    if let value = opt {
        var moved = move opt;
        println(*value);
        if moved {}
    }
}
