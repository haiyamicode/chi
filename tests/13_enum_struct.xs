import "std/ops" as ops;

enum Node (type: uint64) {
    VarDecl,
    FnDef {
        params: Array<string>;
        ret: string;
    };

    struct {
        name: string = "";

        func greeting() string {
            return stringf("hello, {}", this.name);
        }

        func is_callable() bool {
            return switch this {
                Node.FnDef => true,
                else => false
            };
        }

        func type_name() string {
            return this.display();
        }

        func get_ret_or_default() string {
            return switch this {
                Node.FnDef => this.ret,
                else => "none"
            };
        }

        func describe() string {
            return switch this {
                Node.VarDecl, Node.FnDef => this.name,
                else => "unknown"
            };
        }
    }
}

struct Traced {
    id: int = 0;

    mut func new(id: int) {
        this.id = id;
        printf("Traced({}) created\n", id);
    }

    mut func delete() {
        printf("Traced({}) destroyed\n", this.id);
    }

    impl ops.CopyFrom<Traced> {
        mut func copy_from(source: &Traced) {
            this.id = source.id;
            printf("Traced({}) copied\n", source.id);
        }
    }
}

enum TaggedValue (type: int) {
    Empty,
    Named {
        label: string;
    },
    Tracked {
        trace: Traced;
    };

    struct {
        id: int = 0;
    }
}

// Generic enum: single type param used in variants and base struct
enum Container<T> {
    Empty,
    Single {
        value: T;
    },
    Pair {
        first: T;
        second: T;
    };

    struct {
        label: string = "";

        func get_label() string {
            return this.label;
        }

        func is_single() bool {
            return switch this {
                Container.Single => true,
                else => false
            };
        }
    }
}

// Generic enum: multiple type params
enum Either<L, R> {
    Left {
        left: L;
    },
    Right {
        right: R;
    }
}

// Generic enum: type params with discriminator
enum Tagged<T> (tag: uint64) {
    First,
    Second {
        payload: T;
    }
}

func test_generic_enum() {
    println("=== Test: Generic enum ===");

    // Single variant with type param field + base struct method
    var a = Container<int>.Single{label: "one", value: 42};
    printf("a.value={}\n", a.value);
    printf("a.get_label()={}\n", a.get_label());
    printf("a.is_single()={}\n", a.is_single());

    // Pair variant with two type param fields
    var b = Container<int>.Pair{
        label: "pair",
        first: 10,
        second: 20
    };
    printf("b.first={}\n", b.first);
    printf("b.second={}\n", b.second);
    printf("b.get_label()={}\n", b.get_label());
    printf("b.is_single()={}\n", b.is_single());

    // Empty variant (no type param fields)
    var c = Container<int>.Empty{label: "empty"};
    printf("c.label={}\n", c.label);

    // Different type instantiation
    var d = Container<string>.Single{label: "str", value: "hello"};
    printf("d.value={}\n", d.value);
    printf("d.get_label()={}\n", d.get_label());
}

func test_enum_string_copy() {
    println("=== Test: Enum string copy ===");
    var s = TaggedValue.Named{id: 1, label: stringf("name_{}", 42)};
    printf("s.label={}\n", s.label);
    var t = s;
    printf("t.label={}\n", t.label);
    s = {id: 2, label: stringf("name_{}", 99)};
    printf("s.label={}, t.label={}\n", s.label, t.label);
    println("--- scope exit ---");
}

func test_enum_traced_copy() {
    println("=== Test: Enum traced copy ===");
    var a = TaggedValue.Tracked{id: 1, trace: Traced{10}};
    printf("a.trace.id={}\n", a.trace.id);
    var b = a;
    printf("b.trace.id={}\n", b.trace.id);
    println("--- scope exit ---");
}

func test_enum_traced_reassign() {
    println("=== Test: Enum traced reassign ===");
    var a = TaggedValue.Tracked{id: 1, trace: Traced{10}};
    a = {id: 2, trace: Traced{20}};
    printf("a.trace.id={}\n", a.trace.id);
    println("--- scope exit ---");
}

func test_enum_trivial_copy() {
    println("=== Test: Enum trivial variant copy ===");
    var a = TaggedValue.Empty{id: 5};
    var b = a;
    printf("a.id={}, b.id={}\n", a.id, b.id);
    println("--- scope exit ---");
}

func test_enum_string_reassign() {
    println("=== Test: Enum string reassign ===");
    var a = TaggedValue.Named{id: 1, label: stringf("name_{}", 55)};
    printf("a.label={}\n", a.label);
    a = {id: 2, label: stringf("name_{}", 77)};
    printf("a.label={}\n", a.label);
    println("--- scope exit ---");
}

func test_switch_narrowing() {
    println("=== Test: Switch narrowing ===");

    // Method using narrowing to access variant fields
    var fn = Node.FnDef{
        name: "add",
        params: {},
        ret: "int"
    };
    println(fn.get_ret_or_default());

    var vd = Node.VarDecl{name: "x"};
    println(vd.get_ret_or_default());

    // Multi-clause case should not narrow (returns base field only)
    println(fn.describe());
    println(vd.describe());
}

func test_node_copy() {
    println("=== Test: Node copy ===");
    var node = Node.FnDef{
        name: "f",
        params: {},
        ret: "int"
    };
    var copy = node;
    printf("copy.ret={}\n", copy.ret);
    printf("copy.name={}\n", copy.name);
    println("--- scope exit ---");
}

// Enum with custom Display override (no variants)
enum Color {
    Red,
    Green,
    Blue;

    struct {
        impl ops.Display {
            func display() string {
                return switch this {
                    Color.Red => "red",
                    Color.Green => "green",
                    Color.Blue => "blue"
                };
            }
        }
    }
}

// Enum struct with Display + variant data
enum Shape {
    Circle {
        radius: float64;
    },
    Rect {
        w: float64;
        h: float64;
    };

    struct {
        impl ops.Display {
            func display() string {
                return switch this {
                    Shape.Circle => stringf("circle(r={})", this.radius),
                    Shape.Rect => stringf("rect({}x{})", this.w, this.h)
                };
            }
        }
    }
}

// Enum with non-trivial base struct field (lifecycle: copy, destroy)
enum LifecycleEnum {
    Alpha,
    Beta {
        extra: int;
    };

    struct {
        trace: Traced;
    }
}

func test_enum_base_struct_lifecycle() {
    println("=== Test: Enum base struct lifecycle ===");

    // Create: Traced in base struct is constructed
    println("-- create --");
    var a = LifecycleEnum.Alpha{trace: Traced{1}};
    printf("a.trace.id={}\n", a.trace.id);

    // Copy: base struct Traced is copied
    println("-- copy --");
    var b = a;
    printf("b.trace.id={}\n", b.trace.id);

    // Reassign: old Traced destroyed, new one created
    println("-- reassign --");
    a = {trace: Traced{2}};
    printf("a.trace.id={}\n", a.trace.id);

    // Variant with extra data + base struct field
    println("-- variant with data --");
    var c = LifecycleEnum.Beta{trace: Traced{3}, extra: 99};
    printf("c.trace.id={}, c.extra={}\n", c.trace.id, c.extra);

    // Copy variant with data
    println("-- copy variant --");
    var d = c;
    printf("d.trace.id={}, d.extra={}\n", d.trace.id, d.extra);

    // Scope exit: all destroyed
    println("-- scope exit --");
}

func test_enum_display_override() {
    println("=== Test: Enum Display override ===");

    // Basic enum: printf uses custom Display, not default "Color.Red"
    var r = Color.Red;
    var g = Color.Green;
    var b = Color.Blue;
    printf("printf red: {}\n", r);
    printf("printf green: {}\n", g);
    printf("printf blue: {}\n", b);

    // Direct .display() must match printf output
    printf("display red: {}\n", r.display());
    printf("display green: {}\n", g.display());
    printf("display blue: {}\n", b.display());

    // Enum with variant data
    var c = Shape.Circle{radius: 3.14};
    var rect = Shape.Rect{w: 10.0, h: 20.0};
    printf("printf circle: {}\n", c);
    printf("printf rect: {}\n", rect);
    printf("display circle: {}\n", c.display());
    printf("display rect: {}\n", rect.display());

    // Copy preserves Display behavior
    var r2 = r;
    printf("copy printf: {}\n", r2);
    printf("copy display: {}\n", r2.display());

    // Reassign and display
    r2 = Color.Blue;
    printf("reassign printf: {}\n", r2);
}

func test_switch_statement() {
    println("=== Test: Switch statement ===");

    var c = Color.Green;

    // Statement switch: no semicolon, no else, non-exhaustive
    switch c {
        Color.Green => println("green stmt")
    }

    // Statement switch: exhaustive, no else
    switch c {
        Color.Red => println("red"),
        Color.Green => println("green"),
        Color.Blue => println("blue")
    }

    // Expression switch: exhaustive, no else needed
    var name = switch c {
        Color.Red => "red",
        Color.Green => "green",
        Color.Blue => "blue"
    };
    printf("expr: {}\n", name);
}

func main() {
    var node = Node.FnDef{
        name: "f",
        params: {},
        ret: "int"
    };
    printf("node.type: {}\n", node.type);
    printf("node.discriminator: {}\n", node.discriminator());
    printf("node.ret: {}\n", node.ret);
    printf("node.name: {}\n", node.name);
    printf("greeting: {}\n", node.greeting());
    printf("discriminator value: {}\n", node.discriminator());
    printf("is_callable: {}\n", node.is_callable());
    printf("type_name: {}\n", node.type_name());
    test_generic_enum();
    test_enum_string_copy();
    test_enum_traced_copy();
    test_enum_traced_reassign();
    test_enum_trivial_copy();
    test_enum_string_reassign();
    test_node_copy();
    test_switch_narrowing();
    test_enum_base_struct_lifecycle();
    test_enum_display_override();
    test_switch_statement();
}

