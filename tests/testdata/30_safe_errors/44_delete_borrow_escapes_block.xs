// Borrow escapes inner block; explicit cast creates the borrow.
// expect-error: used after delete

struct Data {
    value: int = 0;
}

func main() {
    var x = new Data{value: 42};
    var r: &Data = x as &Data;
    unsafe {
        delete x;
    }
    printf("{}\n", r.value);
}
