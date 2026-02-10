struct Bar {
    id: int = 0;

    mut func new(id: int) {
        this.id = id;
    }

    func delete() {
        printf("delete bar {}\n", this.id);
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

func main() {
    test_foo();
}

