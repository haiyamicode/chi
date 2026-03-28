// References must not coerce to bool in conditions.
// expect-error: cannot convert from &int to bool

func main() {
    var value = 1;
    let r = &value;
    if r {
        println(*r);
    }
}
