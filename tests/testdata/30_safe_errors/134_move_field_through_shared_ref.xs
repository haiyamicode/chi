// expect-error: cannot move out of a non-owning reference
struct S {
    b: Box<int>;
}

func take(b: Box<int>) { printf("take: {}\n", *b); }

func main() {
    var s = S{b: Box<int>.from_value(42)};
    let r: &S = &s;
    take(move r.b);
}
