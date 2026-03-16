// Borrow after value move — source is sunk.
// expect-error: used after move

struct Obj {
    value: int = 0;
}

func main() {
    var a = Obj{value: 1};
    var b = move a;
    var r: &Obj = &a; // error: 'a' used after move
}
