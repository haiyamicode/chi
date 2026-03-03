// Use after move: explicit &move expression sinks the source
// expect-error: used after move
struct Obj {
    value: int;

    mut func new(v: int) {
        this.value = v;
    }
}

func main() {
    var a = Obj{1};
    var b = &move a; // explicit move, a is sunk
    println(a.value); // error: 'a' used after move
}

