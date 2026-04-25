// Map.get returns ?(&V); inline `!` unwrap must keep the borrow tied to the
// map so a later mutation is rejected.
// expect-error: exclusive access

func main() {
    var m: Map<string, int> = {};
    m.set("target", 12345);
    let r: &int = m.get("target")!;
    m.remove("target");
    printf("after: {}\n", *r);
}
