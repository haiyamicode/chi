import "std/ops" as ops;

struct Bar {
    id: int = 0;

    mut func new(id: int) {
        this.id = id;
    }

    func delete() {
        printf("delete bar {}\n", this.id);
    }

    impl ops.CopyFrom<Bar> {
        func copy_from(source: &Bar) {
            this.id = source.id;
        }
    }
}

struct Foo {
    id: int;
    bar1: Bar;
    bar2: Bar;

    mut func new(id: int) {
        this.id = id;
        this.bar1 = {id * 10 + 1};
        this.bar2 = {id * 10 + 2};
    }

    func delete() {
        printf("delete foo {}\n", this.id);
    }

    impl ops.CopyFrom<Foo> {
        func copy_from(source: &Foo) {
            this.id = source.id;
            this.bar1 = source.bar1;
            this.bar2 = source.bar2;
        }
    }
}

func f2() {
    var foo3 = Foo{3};
    panic("exit");
}

func f() {
    var foo2 = Foo{2};
    f2();
}

func test_foo() {
    var foo1 = Foo{1};
    try f();
    println("finished");
}

func test_assign_construct() {
    var p = new Bar{10};
    printf("before: {}\n", p.id);
    *p = {20};
    printf("after: {}\n", p.id);
    delete p;
}

func main() {
    test_assign_construct();
    test_foo();
}

