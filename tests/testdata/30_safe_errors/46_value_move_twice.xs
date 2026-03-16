// Value move twice: second move uses already-sunk source.
// expect-error: used after move

struct Obj {
    value: int = 0;
}

func main() {
    var a = Obj{value: 1};
    var b = move a;
    var c = move a; // error: 'a' used after move
}
