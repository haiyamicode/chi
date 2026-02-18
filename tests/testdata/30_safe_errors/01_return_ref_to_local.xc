// Return reference to local variable — must be rejected
func bad() &int {
    var local = 42;
    return &local;
}
func main() {}
