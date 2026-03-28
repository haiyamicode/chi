// References must not be comparable to null.
// expect-error: references cannot be compared to null

func main() {
    var value = 1;
    let r = &value;
    println(r == null);
}
