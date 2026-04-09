// expect-error: mut method

struct Elem {
    value: int = 0;
}

func main() {
    var items: Array<Elem> = [];
    items.push(Elem{value: 1});

    let p = items.push;
    p(Elem{value: 2});
}
