// Move in loop body, use after loop.

struct Obj {
    value: int;
}

func consume(o: Obj) {}

func main() {
    var a = Obj{value: 1};
    for var i = 0; i < 1; i++ {
        consume(move a);
    }
    printf("{}\n", a.value);    // error: 'a' used after move
}
