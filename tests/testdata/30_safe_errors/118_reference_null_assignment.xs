// References must not accept null.
// expect-error: cannot convert from null to &int

func main() {
    var r: &int = null;
    println(*r);
}
