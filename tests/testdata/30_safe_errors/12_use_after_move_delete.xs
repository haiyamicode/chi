// Use after move: deleting a moved reference
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
    unsafe {
        delete a; // error: 'a' used after move
    }
}

