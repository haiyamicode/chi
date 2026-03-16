// Move twice: passing the same &move value to two different owners
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
    var b = a; // first move
    var c = a; // error: 'a' used after move
}
