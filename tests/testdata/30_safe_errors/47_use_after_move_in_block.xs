// Value move in inner block, use after block.
// expect-error: used after move

struct Obj {
    value: int = 0;
}

func main() {
    var a = Obj{value: 1};
    {
        var b = move a;
    }
    printf("{}\n", a.value); // error: 'a' used after move
}
