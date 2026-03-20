// Lambda moves a captured outer value, then the outer value is used after the call.
// expect-error: used after move

func main() {
    var s = stringf("hello {}", "world");
    let f = func() string {
        return move s;
    };
    println(f());
    println(s);
}
