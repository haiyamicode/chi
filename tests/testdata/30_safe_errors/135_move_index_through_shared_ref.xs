// expect-error: cannot move out of a non-owning reference
func take(b: Box<int>) { printf("take: {}\n", *b); }

func main() {
    var arr: Array<Box<int>> = [];
    arr.push(Box<int>.from_value(42));
    let r: &Array<Box<int>> = &arr;
    take(move r[0]);
}
