// expect-error: exclusive access

struct Elem {
    value: int = 0;
}

func grow(array_ref: &mutex Array<Elem>) {
    array_ref.push(Elem{value: 2});
}

func main() {
    var items: Array<Elem> = [];
    items.push(Elem{value: 1});

    let r = &items[0];
    grow(&mutex items);
    printf("{}\n", r.value);
}
