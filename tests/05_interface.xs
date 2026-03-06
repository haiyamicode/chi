import "std/ops" as ops;

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

    mut func delete() {
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

    impl ops.CopyFrom<Circle> {
        mut func copy_from(source: &Circle) {
            this.radius = source.radius;
        }
    }
}

struct Rect {
    w: int = 0;
    h: int = 0;

    mut func delete() {
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

    impl ops.CopyFrom<Rect> {
        mut func copy_from(source: &Rect) {
            this.w = source.w;
            this.h = source.h;
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
    var ant = Ant{base: {3}};
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

// --- Multi-interface dispatch ---

interface Describable {
    func describe() string;
}

interface Measurable {
    func measure() int;
}

struct Widget {
    label: string = "";
    size: int = 0;

    impl Describable, Measurable {
        func describe() string {
            return stringf("Widget({})", this.label);
        }

        func measure() int {
            return this.size;
        }
    }
}

func show_desc(d: &Describable) string {
    return d.describe();
}

func show_measure(m: &Measurable) int {
    return m.measure();
}

func test_multi_interface() {
    println("=== multi-interface dispatch ===");
    var w = Widget{label: "btn", size: 42};

    // Direct calls
    printf("direct: {} size={}\n", w.describe(), w.measure());

    // Dispatch through first interface
    printf("desc: {}\n", show_desc(&w));

    // Dispatch through second interface
    printf("measure: {}\n", show_measure(&w));

    // Interface ref variables
    var d: &Describable = &w;
    var m: &Measurable = &w;
    printf("ref desc: {}\n", d.describe());
    printf("ref measure: {}\n", m.measure());
}

func describe_shape(s: &Shape) {
    switch s.(type) {
        &Circle => printf("circle r={}\n", s.radius),
        &Rect => printf("rect {}x{}\n", s.w, s.h),
        else => println("unknown shape")
    }
}

func describe_animal(a: &Animal) string {
    return switch a.(type) {
        &Sheep => "sheep",
        &Cat => "cat",
        else => "other"
    };
}

func test_type_switch() {
    println("=== type switch ===");
    var c = Circle{radius: 8};
    var r = Rect{w: 5, h: 3};
    describe_shape(&c);
    describe_shape(&r);

    var sheep = Sheep{10};
    var cat = Cat{20};
    var ant = Ant{base: {30}};
    printf("sheep: {}\n", describe_animal(&sheep));
    printf("cat: {}\n", describe_animal(&cat));
    printf("ant: {}\n", describe_animal(&ant));
}

// --- Default interface methods ---

interface Greeter {
    func name() string;

    // Default method calling abstract method
    func greet() string {
        return "Hello, " + this.name() + "!";
    }

    // Default method calling another default method
    func shout() string {
        return this.greet() + "!!";
    }
}

struct Person {
    _name: string = "";

    impl Greeter {
        func name() string {
            return this._name;
        }
    }
}

// inherits greet() and shout() defaults

struct Robot {
    _name: string = "";

    impl Greeter {
        func name() string {
            return this._name;
        }

        // override greet() but inherit shout()
        func greet() string {
            return "Beep, I am " + this.name();
        }
    }
}

struct Alien {
    _name: string = "";

    impl Greeter {
        func name() string {
            return this._name;
        }

        // override shout() but inherit greet()
        func shout() string {
            return this.name() + " says GREETINGS";
        }
    }
}

func greet_via_ref(g: &Greeter) string {
    return g.greet();
}

func shout_via_ref(g: &Greeter) string {
    return g.shout();
}

func test_default_methods() {
    println("=== default methods ===");

    // Direct calls — Person uses both defaults
    var p = Person{_name: "Alice"};
    printf("person greet: {}\n", p.greet());
    printf("person shout: {}\n", p.shout());

    // Direct calls — Robot overrides greet, inherits shout (which calls greet)
    var r = Robot{_name: "R2D2"};
    printf("robot greet: {}\n", r.greet());
    printf("robot shout: {}\n", r.shout());

    // Direct calls — Alien inherits greet, overrides shout
    var a = Alien{_name: "Zorg"};
    printf("alien greet: {}\n", a.greet());
    printf("alien shout: {}\n", a.shout());

    // Vtable dispatch through &Greeter
    var g: &Greeter = &p;
    printf("ref person greet: {}\n", g.greet());
    printf("ref person shout: {}\n", g.shout());

    g = &r;
    printf("ref robot greet: {}\n", g.greet());
    printf("ref robot shout: {}\n", g.shout());

    g = &a;
    printf("ref alien greet: {}\n", g.greet());
    printf("ref alien shout: {}\n", g.shout());

    // Through function taking &Greeter
    printf("fn person: {}\n", greet_via_ref(&p));
    printf("fn robot: {}\n", greet_via_ref(&r));
    printf("fn alien shout: {}\n", shout_via_ref(&a));
}

// Default method with multiple interfaces
interface Named {
    func label() string;

    func full_label() string {
        return "[" + this.label() + "]";
    }
}

struct Item {
    tag: string = "";

    impl Named, Describable {
        func label() string {
            return this.tag;
        }

        func describe() string {
            return stringf("Item({})", this.tag);
        }
    }
}

func test_default_multi_interface() {
    println("=== default + multi-interface ===");
    var item = Item{tag: "sword"};

    // Direct: default from Named, concrete from Describable
    printf("label: {}\n", item.full_label());
    printf("desc: {}\n", item.describe());

    // Dispatch through each interface
    var n: &Named = &item;
    printf("ref label: {}\n", n.full_label());

    var d: &Describable = &item;
    printf("ref desc: {}\n", d.describe());
}

// --- Interface embedding (interface-into-interface) ---

interface Greetable {
    func greet() string;
}

interface Farewellable {
    func bye() string;
}

// Composite: embeds two interfaces
interface Polite {
    ...Greetable;
    ...Farewellable;
}

struct Gentleman {
    name: string = "";

    impl Polite {
        func greet() string {
            return "Hello from " + this.name;
        }
        func bye() string {
            return "Goodbye from " + this.name;
        }
    }
}

// Generic function using composite interface as bound
func be_polite<T: Polite>(p: &T) string {
    return p.greet() + ", " + p.bye();
}

// Multi-level: Level1 -> Level2 -> Level3
interface HasId {
    func id() int;
}

interface HasName {
    func get_name() string;
}

interface Identifiable {
    ...HasId;
    ...HasName;
}

interface Printable {
    func print_info() string;
}

// 3 levels deep
interface FullEntity {
    ...Identifiable;
    ...Printable;
}

struct Entity {
    _id: int = 0;
    _name: string = "";

    impl FullEntity {
        func id() int {
            return this._id;
        }
        func get_name() string {
            return this._name;
        }
        func print_info() string {
            return stringf("Entity({}, {})", this._id, this._name);
        }
    }
}

func test_interface_embedding() {
    println("=== interface embedding ===");

    // Direct calls on struct implementing composite interface
    var g = Gentleman{name: "Bob"};
    printf("greet: {}\n", g.greet());
    printf("bye: {}\n", g.bye());

    // Through composite interface ref
    var p: &Polite = &g;
    printf("ref greet: {}\n", p.greet());
    printf("ref bye: {}\n", p.bye());

    // Generic with composite bound
    printf("generic polite: {}\n", be_polite<Gentleman>(&g));
}

func show_entity<T: FullEntity>(e: &T) string {
    return stringf("#{}: {} - {}", e.id(), e.get_name(), e.print_info());
}

func sum_three<T: ops.Number>(a: T, b: T, c: T) T {
    return a + b + c;
}

func bitwise_and<T: ops.Int>(a: T, b: T) T {
    return a & b;
}

func test_multilevel_embedding() {
    println("=== multi-level embedding ===");

    var e = Entity{_id: 42, _name: "hero"};

    // Direct calls
    printf("id: {}\n", e.id());
    printf("name: {}\n", e.get_name());
    printf("info: {}\n", e.print_info());

    // Through top-level composite ref
    var fe: &FullEntity = &e;
    printf("ref id: {}\n", fe.id());
    printf("ref name: {}\n", fe.get_name());
    printf("ref info: {}\n", fe.print_info());

    // Generic with multi-level composite bound
    printf("generic entity: {}\n", show_entity<Entity>(&e));
}

// --- Struct embedding tests ---

interface Adder {
    func add(n: int) int;
}

struct EmbedBase {
    val: int = 0;

    mut func new(v: int) {
        this.val = v;
    }

    func doubled() int {
        return this.val * 2;
    }

    impl Adder {
        func add(n: int) int {
            return this.val + n;
        }
    }
}

// Single-level embedding: proxies forward to EmbedBase methods
struct EmbedDerived {
    ...base: EmbedBase;

    mut func new(v: int) {
        this.base = {v};
    }
}

// Override: only the overridden method uses its own impl; other methods proxy
struct EmbedOverride {
    ...base: EmbedBase;

    mut func new(v: int) {
        this.base = {v};
    }

    func doubled() int {
        return this.val * 10;
    }
}

// Multi-level: EmbedDeep.method proxies to EmbedDerived.method which proxies to EmbedBase.method
struct EmbedDeep {
    ...inner: EmbedDerived;

    mut func new(v: int) {
        this.inner = {v};
    }
}

func via_adder(a: &Adder) int {
    return a.add(100);
}

func test_struct_embed() {
    println("=== struct embedding ===");

    // Direct proxy method calls
    var d = EmbedDerived{5};
    printf("doubled: {}\n", d.doubled());
    printf("add: {}\n", d.add(3));
    printf("val: {}\n", d.val);

    // Override suppresses proxy; non-overridden method still proxied
    var o = EmbedOverride{3};
    printf("overridden doubled: {}\n", o.doubled());
    printf("inherited add: {}\n", o.add(7));

    // Multi-level proxy chain: EmbedDeep -> EmbedDerived -> EmbedBase
    var deep = EmbedDeep{4};
    printf("deep doubled: {}\n", deep.doubled());
    printf("deep add: {}\n", deep.add(6));

    // Interface propagated through embedding; vtable dispatch
    printf("adder d: {}\n", via_adder(&d));
    printf("adder deep: {}\n", via_adder(&deep));
}

func test_ops_composite() {
    println("=== ops composite interfaces ===");

    // Number bound: int satisfies Add+Sub+Mul+Div+Rem+Neg+Eq+Ord
    printf("sum_three int: {}\n", sum_three<int>(10, 20, 30));
    printf("sum_three float: {}\n", sum_three<float>(1.5, 2.5, 3.0));

    // Int bound: includes Number + bitwise ops
    printf("bitwise_and int: {}\n", bitwise_and<int>(255, 15));
}

func main() {
    test_basic();
    test_heap_and_delete();
    test_function_params();
    test_sizeof();
    test_box();
    test_multi_interface();
    test_type_switch();
    test_default_methods();
    test_default_multi_interface();
    test_interface_embedding();
    test_multilevel_embedding();
    test_struct_embed();
    test_ops_composite();
}

