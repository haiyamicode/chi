// Use after move: reassigning via &move sinks the source
// expect-error: used after move
struct Obj {
    value: int;
    func new(v: int) { this.value = v; }
}

func main() {
    var a = new Obj{1};
    var b = new Obj{2};
    b = a;              // a is moved to b
    println(a.value);   // error: 'a' used after move
}
