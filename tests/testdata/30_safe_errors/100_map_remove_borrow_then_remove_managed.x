// expect-error: exclusive access

func main() {
    var m: Map<int, int> = {};
    m.set(1, 10);

    let r = &m[1];
    m.remove(1);
    printf("{}\n", *r);
}
