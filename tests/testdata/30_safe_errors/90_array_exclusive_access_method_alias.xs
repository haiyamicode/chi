// expect-error: exclusive access

struct Elem {
    value: int = 0;

    func touch() int {
        return this.value;
    }

    func exploit(array_ref: &mut Array<Elem>) int {
        array_ref.push(Elem{value: 99});
        return this.touch();
    }
}

func main() {
    var items: Array<Elem> = [];
    items.push(Elem{value: 1});
    printf("{}\n", items[0].exploit(&mut items));
}
