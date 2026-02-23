// Value move in inner block, use after block.
// expect-error: has not been initialized

struct Obj {
    value: int;
}

func main() {
    var a = Obj{value: 1};
    {
        var b = move a;
    }
    printf("{}\n", a.value); // error: 'a' used after move
}

