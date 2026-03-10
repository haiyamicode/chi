// Use after move: passing &move to function sinks the source
// expect-error: used after move
struct Obj {
    value: int;

    mut func new(v: int) {
        this.value = v;
    }
}

func take(ptr: &move Obj) {
    unsafe {
        delete ptr;
    }
}

func main() {
    var a = new Obj{1};
    take(a);
    println(a.value); // error: 'a' used after move
}

