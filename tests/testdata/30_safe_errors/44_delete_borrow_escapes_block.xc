// Borrow escapes inner block, source deleted in outer scope.

struct Data {
    value: int = 0;
}

func main() {
    var r: &Data;
    var x = new Data{value: 42};
    {
        r = x;
    }
    delete x;
    printf("{}\n", r.value);
}
