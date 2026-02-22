interface Show {
    func show() string;
}

struct Point {
    x: int = 0;
    y: int = 0;

    impl Show {
        func show() string {
            return string.format("({}, {})", this.x, this.y);
        }
    }
}

struct Number {
    value: int = 0;

    impl Show {
        func show() string {
            return string.format("Number({})", this.value);
        }
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

struct ShowBox<T> {
    item: T = {};

    impl where T: Show {
        func show_item() string {
            return this.item.show();
        }
    }

    func get() T {
        return this.item;
    }
}

struct WherePair<T, U> {
    first: T = {};
    second: U = {};

    impl where T: Show, U: Show {
        func show_both() string {
            return string.format("({}, {})", this.first.show(), this.second.show());
        }
    }

    func get_first() T {
        return this.first;
    }
}

struct ImplWhereBox<T> {
    item: T = {};

    impl where T: Show {
        func show_item() string {
            return this.item.show();
        }

        static func describe(val: T) string {
            return string.format("ImplWhereBox[{}]", val.show());
        }
    }

    func get() T {
        return this.item;
    }
}

import "std/ops" as ops;

func sized_identity<T: ops.Sized>(v: T) T {
    return v;
}

struct SizedBox<T: ops.Sized> {
    value: T = {};

    func get() T {
        return this.value;
    }
}

func main() {
    printf("=== Type Parameter Trait Bounds Test ===\n");
    printf("\n-- Function trait bounds --\n");
    var p = Point{};
    print_it(p);
    printf("\n-- Reference trait bounds --\n");
    print_ref(&p);
    printf("\n-- Struct trait bounds --\n");
    var p2 = Point{x: 10, y: 20};
    var container = Container<Point>{item: p2, name: "PointContainer"};
    printf("Single trait bound: {}\n", container.show_container());
    var retrieved = container.get_item();
    printf("Retrieved item: {}\n", retrieved.show());
    var n = Number{value: 42};
    var pair = Pair<Point, Number>{first: p2, second: n};
    printf("Multiple type params: {}\n", pair.show_both());
    var num_container = Container<Number>{item: n, name: "NumberContainer"};
    printf("Different type, same interface: {}\n", num_container.show_container());

    printf("\n-- Where block --\n");
    var sb1 = ShowBox<Point>{item: Point{x: 3, y: 4}};
    printf("show_item: {}\n", sb1.show_item());
    printf("get point: ({}, {})\n", sb1.get().x, sb1.get().y);
    var sb2 = ShowBox<int>{item: 99};
    printf("get int: {}\n", sb2.get());
    var sb3 = ShowBox<Number>{item: Number{value: 7}};
    printf("show_item Number: {}\n", sb3.show_item());
    var wp = WherePair<Point, Number>{first: Point{x: 5, y: 6}, second: Number{value: 8}};
    printf("multi where: {}\n", wp.show_both());
    var wp2 = WherePair<int, Number>{first: 10, second: Number{value: 9}};
    printf("partial get_first: {}\n", wp2.get_first());
    var iwb = ImplWhereBox<Point>{item: Point{x: 7, y: 8}};
    printf("impl where show: {}\n", iwb.show_item());
    var iwb2 = ImplWhereBox<int>{item: 55};
    printf("impl where get: {}\n", iwb2.get());
    var desc_p = Point{x: 9, y: 10};
    printf("static where: {}\n", ImplWhereBox<Point>.describe(desc_p));

    printf("\n-- Sized trait bound --\n");
    printf("int: {}\n", sized_identity(42));
    printf("string: {}\n", sized_identity("hello"));
    printf("bool: {}\n", sized_identity(true));
    printf("float: {}\n", sized_identity(3.14));
    var sp: Point = sized_identity(Point{x: 5, y: 10});
    printf("struct: ({}, {})\n", sp.x, sp.y);
    var sb = SizedBox<int>{value: 99};
    printf("SizedBox<int>: {}\n", sb.get());

    printf("\n=== All trait bound tests passed! ===\n");
}

