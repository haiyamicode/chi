// expect-error: exclusive access

struct Elem {
    value: int = 0;
}

func main() {
    var items: Array<Elem> = [];
    items.push(Elem{value: 1});

    let r = &items[0];
    items.push(Elem{value: 2});
    printf("{}\n", r.value);
}
