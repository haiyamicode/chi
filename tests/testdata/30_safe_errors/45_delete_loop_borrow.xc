// Borrow created in loop body, source deleted after loop.

struct Data {
    value: int = 0;
}

func main() {
    var x = new Data{value: 42};
    var r: &Data;
    for var i = 0; i < 1; i++ {
        r = x;
    }
    delete x;
    printf("{}\n", r.value);
}
