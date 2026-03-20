// Borrow escapes inner block, source deleted in outer scope.
// expect-error: used after delete

struct Data {
    value: int = 0;
}

func main() {
    var x = new Data{value: 42};
    var r: &Data;
    {
        r = x;
    }
    unsafe {
        delete x;
    }
    printf("{}\n", r.value);
}
