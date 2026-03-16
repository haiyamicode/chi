// Use after move: assigning &move T to another &move variable sinks the source
// expect-error: used after move
struct Obj {
    value: int;

    mut func new(v: int) {
        this.value = v;
    }
}

func main() {
    var a = new Obj{1};
    var b = a; // a is moved to b
    println(a.value); // error: 'a' used after move
}
