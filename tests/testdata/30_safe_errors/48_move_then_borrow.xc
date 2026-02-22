// Borrow after value move — source is sunk.

struct Obj {
    value: int;
}

func main() {
    var a = Obj{value: 1};
    var b = move a;
    var r: &Obj = &a;   // error: 'a' used after move
}
