// Use after move: passing &move to function sinks the source
struct Obj {
    value: int;
    func new(v: int) { this.value = v; }
}

func take(ptr: &move Obj) {
    delete ptr;
}

func main() {
    var a = new Obj{1};
    take(a);
    println(a.value);  // error: 'a' used after move
}
