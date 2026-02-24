interface Animal {
    func make_sound();
}

struct AnimalBase {
    id: int;

    mut func new(id: int) {
        this.id = id;
    }

    impl Animal {
        func make_sound() {
            printf("{}: <silence>\n", this.id);
        }
    }
}

struct Ant {
    ...base: AnimalBase;
}

struct Sheep {
    ...base: AnimalBase;

    mut func new(id: int) {
        this.base = {id};
    }

    func make_sound() {
        printf("{}: baaaaahh\n", this.id);
    }
}

struct Cat {
    id: int;

    mut func new(id: int) {
        this.id = id;
    }

    impl Animal {
        func make_sound() {
            printf("{}: meeoooww\n", this.id);
        }
    }
}

// --- Additional types for extended tests ---

interface Shape {
    func area() int;
    func name() string;
}

struct Circle {
    radius: int = 0;

    func delete() {
        printf("Circle.delete(r={})\n", this.radius);
    }

    impl Shape {
        func area() int {
            return this.radius * this.radius * 3;
        }

        func name() string {
            return "circle";
        }
    }
}

struct Rect {
    w: int = 0;
    h: int = 0;

    func delete() {
        printf("Rect.delete({}x{})\n", this.w, this.h);
    }

    impl Shape {
        func area() int {
            return this.w * this.h;
        }

        func name() string {
            return "rect";
        }
    }
}

// Pass interface reference as function parameter
func print_shape(s: &Shape) {
    printf("{}: area={}\n", s.name(), s.area());
}

// Return interface reference
func bigger(a: &Shape, b: &Shape) &Shape {
    unsafe {
        if a.area() > b.area() {
            return a;
        }
        return b;
    }
}

func test_basic() {
    println("=== basic dispatch ===");
    var sheep = Sheep{1};
    var cat = Cat{2};
    var ant = Ant{3};
    var animal: &Animal = &sheep;
    animal.make_sound();
    animal = &cat;
    animal.make_sound();
    animal = &ant;
    animal.make_sound();
}

func test_heap_and_delete() {
    println("=== heap + delete ===");
    var c = new Circle{radius: 5};
    var r = new Rect{w: 3, h: 4};
    var s: &Shape = c;
    printf("{}: area={}\n", s.name(), s.area());
    s = r;
    printf("{}: area={}\n", s.name(), s.area());
    delete c;
    delete r;
    var d = new Circle{radius: 9};
    var si: &Shape = d;
    printf("{}: area={}\n", si.name(), si.area());
    delete d;
}

func test_function_params() {
    println("=== function params ===");
    var c = Circle{radius: 10};
    var r = Rect{w: 6, h: 7};
    print_shape(&c);
    print_shape(&r);
    var b = bigger(&c, &r);
    printf("bigger: {}\n", b.name());
}

func test_sizeof() {
    println("=== sizeof ===");
    var c = Circle{radius: 1};
    var r = Rect{w: 1, h: 1};
    var s: &Shape = &c;
    printf("sizeof &Shape: {}\n", sizeof s);
    printf("sizeof Circle: {}\n", sizeof *s);
    s = &r;
    printf("sizeof Rect: {}\n", sizeof *s);
}

func test_box() {
    println("=== Box<Shape> ===");
    var c = new Circle{radius: 7};
    var b = Box<Shape>{c};
    printf("{}: area={}\n", b.as_ref().name(), b.as_ref().area());

    var r = new Rect{w: 2, h: 3};
    var b2 = Box<Shape>{r};
    printf("{}: area={}\n", b2.as_ref().name(), b2.as_ref().area());
    println("before cleanup:");
}

func main() {
    test_basic();
    test_heap_and_delete();
    test_function_params();
    test_sizeof();
    test_box();
}

