// Borrow created in loop body, source deleted after loop.
// expect-error: used after delete

struct Data {
    value: int = 0;
}

func main() {
    var x = new Data{value: 42};
    var r: &Data;
    for i in 0..1 {
        r = x;
    }
    unsafe { delete x; }
    printf("{}\n", r.value);
}

