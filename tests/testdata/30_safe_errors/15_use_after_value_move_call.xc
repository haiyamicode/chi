// Use after value move passed to function
// expect-error: has not been initialized
struct Obj {
    value: int;
}

func consume(o: Obj) {
    println(o.value);
}

func main() {
    var a = Obj{value: 1};
    consume(move a); // value move into function arg
    println(a.value); // error: 'a' used after move
}

