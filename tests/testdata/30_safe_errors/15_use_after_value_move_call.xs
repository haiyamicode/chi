// Use after value move passed to function
// expect-error: used after move
struct Obj {
    value: int = 0;
}

func consume(o: Obj) {
    println(o.value);
}

func main() {
    var a = Obj{value: 1};
    consume(move a); // value move into function arg
    println(a.value); // error: 'a' used after move
}

