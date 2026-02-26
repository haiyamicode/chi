// expect-error: pack expansion '...' can only be used on a variadic parameter
func main() {
    var x = 10;
    printf(x...);
}
