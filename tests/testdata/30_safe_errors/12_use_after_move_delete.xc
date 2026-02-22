// Use after move: deleting a moved reference
// expect-error: used after move
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
    delete a;           // error: 'a' used after move
}
