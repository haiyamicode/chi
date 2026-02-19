// Use after value move: move x sinks the source
struct Obj {
    value: int;
}

func main() {
    var a = Obj{value: 1};
    var b = move a;     // value move, a is sunk
    println(a.value);   // error: 'a' used after move
}
