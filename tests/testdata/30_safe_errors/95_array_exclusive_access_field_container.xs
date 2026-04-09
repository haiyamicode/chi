// expect-error: exclusive access

struct Elem {
    value: int = 0;
}

struct Holder {
    items: Array<Elem> = [];

    mut func grow() {
        this.items.push(Elem{value: 2});
    }
}

func main() {
    var holder = Holder{};
    holder.items.push(Elem{value: 1});

    let r = &holder.items[0];
    holder.grow();
    printf("{}\n", r.value);
}
