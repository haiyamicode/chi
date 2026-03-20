// Lambda may move a captured outer value on one path, then the outer value is used.
// expect-error: used after move

func main() {
    var flag = true;
    var s = stringf("hello {}", "world");
    let f = func() {
        if flag {
            let _ = move s;
        }
    };
    f();
    println(s);
}
