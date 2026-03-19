// expect-error: exclusive access

struct Elem {
    value: int = 0;
}

func main() {
    var items: Array<Elem> = [];
    items.push(Elem{value: 1});

    let r = &items[0];
    let array_ref = &mutex items;
    array_ref.push(Elem{value: 2});
    printf("{}\n", r.value);
}
