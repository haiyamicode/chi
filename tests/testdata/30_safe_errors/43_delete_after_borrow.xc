// Delete while borrow is still live in the same scope.

struct Data {
    value: int = 0;
}

func main() {
    var x = new Data{value: 42};
    var r: &Data = x;
    delete x;
    printf("{}\n", r.value);
}
