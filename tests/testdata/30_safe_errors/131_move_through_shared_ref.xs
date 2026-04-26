// expect-error: cannot move out of a non-owning reference
func take(b: Box<int>) { printf("take: {}\n", *b); }

func main() {
    var b: Box<int> = Box<int>.from_value(42);
    let r: &Box<int> = &b;
    take(move *r);
}
