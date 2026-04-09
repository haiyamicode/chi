// expect-error: borrow used after exclusive access

struct Elem {
    value: int = 0;
}

func read(p: &Elem) {
    printf("{}\n", p.value);
}

func main() {
    var items: Array<Elem> = [];
    items.push(Elem{value: 1});

    // Taking a mut method as a value synthesizes a lambda that captures
    // the receiver by exclusive reference. Calling that lambda must be
    // treated as an exclusive-access call that invalidates outstanding
    // borrows of the same receiver.
    var p = &items[0];
    let f = items.push;
    f(Elem{value: 2});
    read(p);
}
