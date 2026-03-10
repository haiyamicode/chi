// Delete while borrow is still live in the same scope.
// expect-error: used after delete

struct Data {
    value: int = 0;
}

func main() {
    var x = new Data{value: 42};
    var r: &Data = x;
    unsafe { delete x; }
    printf("{}\n", r.value);
}

