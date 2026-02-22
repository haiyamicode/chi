// Value move twice: second move uses already-sunk source.
// expect-error: has not been initialized

struct Obj {
    value: int;
}

func main() {
    var a = Obj{value: 1};
    var b = move a;
    var c = move a;     // error: 'a' used after move
}
