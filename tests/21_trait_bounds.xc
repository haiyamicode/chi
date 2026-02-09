interface Show {
    func show() string;
}

struct Point implements Show {
    x: int = 0;
    y: int = 0;

    func show() string {
        return string.format("({}, {})", this.x, this.y);
    }
}

struct Number implements Show {
    value: int = 0;

    func show() string {
        return string.format("Number({})", this.value);
    }
}

func print_it<T: Show>(t: T) {
    printf("Result: {}\n", t.show());
}

func print_ref<T: Show>(t: &T) {
    printf("Reference result: {}\n", t.show());
}

struct Container<T: Show> {
    item: T = {};
    name: string = "";

    func show_container() string {
        return string.format("Container[{}]: {}", this.name, this.item.show());
    }

    func get_item() T {
        return this.item;
    }
}

struct Pair<T: Show, U: Show> {
    first: T = {};
    second: U = {};

    func show_both() string {
        return string.format("Pair({}, {})", this.first.show(), this.second.show());
    }
}

func main() {
    printf("=== Type Parameter Trait Bounds Test ===\n");
    printf("\n-- Function trait bounds --\n");
    var p: Point = {};
    print_it(p);
    printf("\n-- Reference trait bounds --\n");
    print_ref(&p);
    printf("\n-- Struct trait bounds --\n");
    var p2: Point = {.x = 10, .y = 20};
    var container: Container<Point> = {.item = p2, .name = "PointContainer"};
    printf("Single trait bound: {}\n", container.show_container());
    var retrieved = container.get_item();
    printf("Retrieved item: {}\n", retrieved.show());
    var n: Number = {.value = 42};
    var pair: Pair<Point, Number> = {.first = p2, .second = n};
    printf("Multiple type params: {}\n", pair.show_both());
    var num_container: Container<Number> = {.item = n, .name = "NumberContainer"};
    printf("Different type, same interface: {}\n", num_container.show_container());
    printf("\n=== All trait bound tests passed! ===\n");
}

