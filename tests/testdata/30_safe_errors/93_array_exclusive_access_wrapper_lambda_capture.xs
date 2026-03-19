// expect-error: exclusive access

struct Elem {
    value: int = 0;
}

func main() {
    var items: Array<Elem> = [];
    items.push(Elem{value: 1});

    let r = &items[0];
    let p = func (e: Elem) {
        items.push(e);
    };
    p(Elem{value: 2});
    printf("{}\n", r.value);
}
