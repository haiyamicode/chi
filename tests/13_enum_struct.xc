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
            return string.format("hello, {}", this.name);
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
    }
}

struct Traced {
    id: int = 0;

    func new(id: int) {
        this.id = id;
        printf("Traced({}) created\n", id);
    }

    func delete() {
        printf("Traced({}) destroyed\n", this.id);
    }

    impl ops.CopyFrom<Traced> {
        func copy_from(source: &Traced) {
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

func test_enum_string_copy() {
    println("=== Test: Enum string copy ===");
    var s = TaggedValue.Named{id: 1, label: string.format("name_{}", 42)};
    printf("s.label={}\n", s.label);
    var t = s;
    printf("t.label={}\n", t.label);
    s = {id: 2, label: string.format("name_{}", 99)};
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
    var a = TaggedValue.Named{id: 1, label: string.format("name_{}", 55)};
    printf("a.label={}\n", a.label);
    a = {id: 2, label: string.format("name_{}", 77)};
    printf("a.label={}\n", a.label);
    println("--- scope exit ---");
}

func test_node_copy() {
    println("=== Test: Node copy ===");
    var node = Node.FnDef{name: "f", params: {}, ret: "int"};
    var copy = node;
    printf("copy.ret={}\n", copy.ret);
    printf("copy.name={}\n", copy.name);
    println("--- scope exit ---");
}

func main() {
    var node = Node.FnDef{name: "f", params: {}, ret: "int"};
    printf("node.type: {}\n", node.type);
    printf("node.discriminator: {}\n", node.discriminator());
    printf("node.ret: {}\n", node.ret);
    printf("node.name: {}\n", node.name);
    printf("greeting: {}\n", node.greeting());
    printf("discriminator value: {}\n", node.discriminator());
    printf("is_callable: {}\n", node.is_callable());
    printf("type_name: {}\n", node.type_name());
    test_enum_string_copy();
    test_enum_traced_copy();
    test_enum_traced_reassign();
    test_enum_trivial_copy();
    test_enum_string_reassign();
    test_node_copy();
}

