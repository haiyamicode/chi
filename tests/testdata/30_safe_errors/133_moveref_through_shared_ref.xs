// expect-error: cannot move out of a non-owning reference
func main() {
    var b: Box<int> = Box<int>.from_value(42);
    let r: &Box<int> = &b;
    let m: &move Box<int> = &move *r;
}
