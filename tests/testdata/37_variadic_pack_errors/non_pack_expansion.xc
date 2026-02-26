// expect-error: pack expansion '...' can only be used on a variadic type pack parameter
func main() {
    var x = 10;
    printf(x...);
}
