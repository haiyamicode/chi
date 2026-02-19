// Use after move: explicit &move expression sinks the source
struct Obj {
    value: int;
    func new(v: int) { this.value = v; }
}

func main() {
    var a = Obj{1};
    var b = &move a;    // explicit move, a is sunk
    println(a.value);   // error: 'a' used after move
}
