import "std/ops" as ops;
import "std/mem" as mem;

struct Foo {
    p: *int = null;
    id: string;

    mut func new(id: string) {
        this.id = id;
        unsafe {
            this.p = mem.malloc(sizeof int);
            mem.memset(this.p, 0, sizeof int);
        }
        printf("creating {}\n", this.id);
    }

    func delete() {
        printf("deleting {}\n", this.id);
        unsafe {
            mem.free(this.p);
        }
    }

    impl ops.CopyFrom<Foo> {
        mut func copy_from(b: &Foo) {
            this.new(string.format("{}_copy", b.id));
            unsafe {
                *this.p = *b.p;
                printf("copied {}, p = {}\n", this.id, *b.p);
            }
        }
    }
}

func return_local() Foo {
    var a = Foo{"local"};
    unsafe {
        *a.p = 42;
    }
    var b = a;
    b.id = "local_b";
    println("return_local() done");
    return b;
}

func return_construct() Foo {
    println("return_construct() returning ConstructExpr");
    return {"direct"};
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

func test_optional_copy() {
    println("=== Test 3: Optional copy (T -> ?T direct) ===");
    var a: ?Traced = Traced{1};
    printf("a.id={}\n", a!.id);

    println("=== Test 4: Optional copy (T -> ?T from var) ===");
    var t = Traced{2};
    var b: ?Traced = t;
    printf("t.id={}, b.id={}\n", t.id, b!.id);

    println("=== Test 5: Optional copy (?T -> ?T) ===");
    var c: ?Traced = a;
    printf("a.id={}, c.id={}\n", a!.id, c!.id);

    println("=== Test 6: Optional reassign ===");
    a = Traced{4};
    printf("a.id={}\n", a!.id);

    println("=== scope exit ===");
}

func main() {
    println("=== Test 1: Return local variable ===");
    var foo = return_local();
    unsafe {
        printf("result: {}\n", *foo.p);
    }
    println("=== Test 2: RVO - return ConstructExpr ===");
    var bar = return_construct();
    unsafe {
        *bar.p = 99;
        printf("result: {}\n", *bar.p);
    }
    println("done");
    test_optional_copy();
}

