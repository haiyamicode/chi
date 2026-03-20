// Nested control flow inside a lambda may move a captured outer value.
// expect-error: used after move

func main() {
    var outer = true;
    var inner = false;
    var s = stringf("hello {}", "world");
    let f = func() {
        if outer {
            if inner {
                println("kept");
            } else {
                let _ = move s;
            }
        }
    };
    f();
    println(s);
}
