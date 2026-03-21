// Explicit cast creates the borrow; using it after delete is invalid.
// expect-error: used after delete

struct Data {
    value: int = 0;
}

func main() {
    var x = new Data{value: 42};
    var r = x as &Data;
    unsafe {
        delete x;
    }
    printf("{}\n", r.value);
}
