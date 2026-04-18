// Implicit owner-to-borrow conversion is only allowed in managed mode or unsafe.
// expect-error: requires an explicit cast

struct Data {
    value: int = 0;
}

func main() {
    var x = new Data{value: 42};
    var r: &Data = x;
    printf("{}\n", r.value);
}
