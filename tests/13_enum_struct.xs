import "std/ops" as ops;

enum Node: uint64 {
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
                FnDef => true,
                else => false
            };
        }

        func type_name() string {
            return this.display();
        }

        func get_ret_or_default() string {
            return switch this {
                FnDef => this.ret,
                else => "none"
            };
        }

        func describe() string {
            return switch this {
                VarDecl, FnDef => this.name,
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

    impl ops.Copy {
        mut func copy(source: &This) {
            this.id = source.id;
            printf("Traced({}) copied\n", source.id);
        }
    }
}


enum TaggedValue: int {
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
                Single => true,
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
enum Tagged<T>: uint64 as tag {
    First,
    Second {
        payload: T;
    }
}

enum TupleResult<T, E> {
    Ok(T),
    Err(E)
}

enum IntExpr {
    Value(int),
    Add(int, int)
}

enum MethodResult<T, E> {
    Ok(T),
    Err(E);

    struct {
        func value() ?T {
            return switch this {
                Ok(value) => value,
                else => null
            };
        }
    }
}


enum TracedResult<T, E> {
    Ok(T),
    Err(E)
}

func make_empty_container<T>(label: string) Container<T> {
    return Empty{:label};
}

func accept_empty_container(value: Container<int>) {
    println("accept empty");
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

    var made: Container<int> = make_empty_container<int>("made");
    printf("made.get_label()={}\n", made.get_label());
    if made.is_single() {
        println("made single");
    } else {
        println("made empty");
    }
    accept_empty_container(made);

    var made2: Container<int> = Container<int>.Single{label: "made2", value: 99};
    printf("made2.get_label()={}\n", made2.get_label());
    if made2.is_single() {
        println("made2 single");
    } else {
        println("made2 empty");
    }

    // Different type instantiation
    var d = Container<string>.Single{label: "str", value: "hello"};
    printf("d.value={}\n", d.value);
    printf("d.get_label()={}\n", d.get_label());
}

func id_wrapbox<T>(value: TupleResult<T, string>) TupleResult<T, string> {
    return value;
}

func test_tuple_enum() {
    println("=== Test: Tuple enum ===");

    var method_ok = MethodResult<int, string>.Ok{42};
    printf("method_ok.has_value={}\n", method_ok.value() != null);
    printf("method_ok.value={}\n", method_ok.value()!);
    printf("method_ok.discriminator={}\n", method_ok.discriminator());

    var method_err = MethodResult<int, string>.Err{"oops"};
    printf("method_err.has_value={}\n", method_err.value() != null);
    printf("method_err.discriminator={}\n", method_err.discriminator());

    var ok = TupleResult<int, string>.Ok{42};
    var ok2 = id_wrapbox<int>(ok);
    var ok3: TupleResult<int, string> = id_wrapbox<int>(ok2);
    var typed_ok: TupleResult<int, string> = TupleResult<int, string>.Ok{99};
    var err = TupleResult<int, string>.Err{"oops"};
    var add = IntExpr.Add{10, 20};

    printf("ok.0={}\n", ok.0);
    println(switch ok2 {
        Ok => "ok2 ok",
        Err => "ok2 err"
    });
    println(switch ok3 {
        Ok => "ok3 ok",
        Err => "ok3 err"
    });
    println(switch typed_ok {
        Ok => "typed ok",
        Err => "typed err"
    });
    printf("err.0={}\n", err.0);
    printf("add.0={}\n", add.0);
    printf("add.1={}\n", add.1);
    var ok_desc = switch ok {
        Ok => stringf("ok {}", ok.0),
        Err => stringf("err {}", ok.0)
    };
    println(ok_desc);

    var err_desc = switch err {
        Ok => stringf("ok {}", err.0),
        Err => stringf("err {}", err.0)
    };
    println(err_desc);

    var sum = switch add {
        Value => add.0,
        Add => add.0 + add.1
    };
    printf("sum={}\n", sum);
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

func test_switch_destructure() {
    println("=== Test: Switch destructure ===");

    var fn = Node.FnDef{
        name: "add",
        params: {},
        ret: "int"
    };
    var fn_desc = switch fn {
        FnDef{name, ret} => stringf("{}:{}", name, ret),
        VarDecl{name} => stringf("{}:var", name)
    };
    println(fn_desc);

    var vd = Node.VarDecl{name: "x"};
    var vd_desc = switch vd {
        FnDef{name, ret} => stringf("{}:{}", name, ret),
        VarDecl{name} => stringf("{}:var", name)
    };
    println(vd_desc);

    var ok = TupleResult<int, string>.Ok{42};
    var err = TupleResult<int, string>.Err{"oops"};
    var add = IntExpr.Add{10, 20};

    var ok_desc = switch ok {
        Ok(value) => stringf("ok {}", value),
        Err(message) => stringf("err {}", message)
    };
    println(ok_desc);

    var err_desc = switch err {
        Ok(value) => stringf("ok {}", value),
        Err(message) => stringf("err {}", message)
    };
    println(err_desc);
}

func test_switch_by_ref_destructure() {
    println("=== Test: Switch by-ref destructure ===");

    var fn = Node.FnDef{
        name: "before",
        params: ["x"],
        ret: "int"
    };
    var changed = switch fn {
        FnDef{&name, &mut ret} => {
            println(*name);
            *ret = "bool";
            *ret
        },
        else => "none"
    };
    println(changed);
    println(fn.ret);
}

func test_switch_nested_destructure() {
    println("=== Test: Switch nested destructure ===");

    var fn = Node.FnDef{
        name: "join",
        params: ["lhs", "rhs", "tail"],
        ret: "txt"
    };
    var desc = switch fn {
        FnDef{name, params: [first, ...rest], ret} => stringf(
            "{}:{}:{}:{}:{}",
            name,
            ret,
            first,
            rest[0],
            rest[1]
        ),
        else => "empty"
    };
    println(desc);
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

func test_bare_variant_shorthand() {
    println("=== Test: Bare variant shorthand ===");

    var node: Node = FnDef{
        name: "outside",
        params: {},
        ret: "bool"
    };
    println(node.get_ret_or_default());

    node = VarDecl{name: "x"};
    println(node.get_ret_or_default());

    var kind = switch node {
        VarDecl => "var",
        FnDef => "fn"
    };
    println(kind);
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
                    Red => "red",
                    Green => "green",
                    Blue => "blue"
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
                    Circle => stringf("circle(r={})", this.radius),
                    Rect => stringf("rect({}x{})", this.w, this.h)
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



func make_traced_result() TracedResult<int, Traced> {
    return TracedResult<int, Traced>.Err{Traced{11}};
}

func test_generic_enum_lifecycle() {
    println("=== Test: Generic enum lifecycle ===");

    var err = make_traced_result();
    println("-- err copy --");
    var forwarded: TracedResult<int, Traced> = err;
    println("-- err overwrite copy --");
    forwarded = TracedResult<int, Traced>.Ok{123};

    var ok = TracedResult<Traced, int>.Ok{Traced{22}};
    println("-- ok copy --");
    var ok_forwarded: TracedResult<Traced, int> = ok;
    println("-- ok overwrite copy --");
    ok_forwarded = TracedResult<Traced, int>.Err{7};

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
        Green => println("green stmt")
    }

    // Statement switch: exhaustive, no else
    switch c {
        Red => println("red"),
        Green => println("green"),
        Blue => println("blue")
    }

    // Expression switch: exhaustive, no else needed
    var name = switch c {
        Red => "red",
        Green => "green",
        Blue => "blue"
    };
    printf("expr: {}\n", name);
}

func main() {
    var node = Node.FnDef{
        name: "f",
        params: {},
        ret: "int"
    };
    printf("node.discriminator value: {}\n", node.discriminator());
    printf("node.discriminator: {}\n", node.discriminator());
    printf("node.ret: {}\n", node.ret);
    printf("node.name: {}\n", node.name);
    printf("greeting: {}\n", node.greeting());
    var tagged = Tagged<int>.Second{payload: 123};
    printf("tagged.tag: {}\n", tagged.tag);
    printf("is_callable: {}\n", node.is_callable());
    printf("type_name: {}\n", node.type_name());
    test_generic_enum();
    test_tuple_enum();
    test_enum_string_copy();
    test_enum_traced_copy();
    test_enum_traced_reassign();
    test_enum_trivial_copy();
    test_enum_string_reassign();
    test_bare_variant_shorthand();
    test_node_copy();
    test_switch_narrowing();
    test_switch_destructure();
    test_switch_by_ref_destructure();
    test_switch_nested_destructure();
    test_enum_base_struct_lifecycle();
    test_generic_enum_lifecycle();
    test_enum_display_override();
    test_switch_statement();
}
