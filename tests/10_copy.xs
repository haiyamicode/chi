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

    mut func delete() {
        printf("deleting {}\n", this.id);
        unsafe {
            mem.free(this.p);
        }
    }

    impl ops.Copy {
        mut func copy(b: &This) {
            this.new(stringf("{}_copy", b.id));
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

struct MoveOnly {
    id: int = 0;

    mut func new(id: int) {
        this.id = id;
        printf("MoveOnly({}) created\n", id);
    }

    mut func delete() {
        printf("MoveOnly({}) destroyed\n", this.id);
    }

    impl ops.NoCopy {}
}

func make_traced(id: int) Traced {
    return Traced{id};
}

func make_move_only(id: int) MoveOnly {
    return MoveOnly{id};
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

    println("=== Test 6: Optional reassign from temp ===");
    a = Traced{4};
    printf("a.id={}\n", a!.id);

    println("=== Test 7: Optional reassign from var ===");
    var t2 = Traced{5};
    a = t2;
    printf("t2.id={}, a.id={}\n", t2.id, a!.id);

    println("=== Test 8: Optional cast conversion matrix ===");

    println("-- named cast to optional --");
    var cast_named = Traced{7};
    var cast_a = cast_named as ?Traced;
    println("scope opt a");

    println("-- move cast to optional --");
    var cast_moved_src = Traced{8};
    var cast_b = (move cast_moved_src) as ?Traced;
    println("scope opt b");

    println("-- temp cast to optional --");
    var cast_c = make_traced(9) as ?Traced;
    println("scope opt c");

    println("-- temp nocopy cast to optional --");
    var cast_d = make_move_only(10) as ?MoveOnly;
    println("scope opt d");

    println("-- move nocopy cast to optional --");
    var cast_moved_nocopy = MoveOnly{11};
    var cast_e = (move cast_moved_nocopy) as ?MoveOnly;
    println("scope opt e");

    println("-- same-type cast optional --");
    var same_opt_src: ?Traced = make_traced(17);
    var same_opt = same_opt_src as ?Traced;
    println("scope same opt");

    println("=== scope exit ===");
}

func test_any_box_and_destroy() {
    println("=== Test 9: Any box + destroy ===");
    var a: any = Traced{1};
    println("--- scope exit ---");
}

func test_any_copy() {
    println("=== Test 10: Any copy ===");
    var a: any = Traced{1};
    var b: any = a;
    println("--- scope exit ---");
}

func test_any_reassign() {
    println("=== Test 11: Any reassign ===");
    var a: any = Traced{1};
    a = Traced{2};
    println("--- scope exit ---");
}

func test_any_move() {
    println("=== Test 12: Any move T -> any ===");
    var t = Traced{3};
    var a: any = move t;
    println("--- scope exit ---");
}

func test_any_string() {
    println("=== Test 13: Any with string ===");
    var s = stringf("hello {}", "world");
    var a: any = s;
    var b: any = a;
    printf("s={}\n", s);
    println("--- scope exit ---");
}

func test_any_int() {
    println("=== Test 14: Any with int (trivial) ===");
    var a: any = 42;
    var b: any = a;
    println("--- scope exit ---");
}

func test_any_conversion_matrix() {
    println("=== Test 15: Any conversion matrix ===");

    println("-- named init to any --");
    var named = Traced{1};
    var a: any = named;
    println("scope a");

    println("-- move init to any --");
    var moved_src = Traced{2};
    var b: any = move moved_src;
    println("scope b");

    println("-- temp init to any --");
    var c: any = Traced{3};
    println("scope c");

    println("-- named assign to any --");
    var assign_named = Traced{4};
    var d: any = 0;
    d = assign_named;
    println("scope d");

    println("-- move assign to any --");
    var assign_move = Traced{5};
    var e: any = 0;
    e = move assign_move;
    println("scope e");

    println("-- temp assign to any --");
    var f: any = 0;
    f = Traced{6};
    println("scope f");
}

func test_any_cast_conversion_matrix() {
    println("=== Test 16: Any cast conversion matrix ===");

    println("-- named cast to any --");
    var named = Traced{12};
    var a = named as any;
    println("scope any a");

    println("-- move cast to any --");
    var moved_src = Traced{13};
    var b = (move moved_src) as any;
    println("scope any b");

    println("-- temp cast to any --");
    var c = make_traced(14) as any;
    println("scope any c");

    println("-- temp nocopy cast to any --");
    var d = make_move_only(15) as any;
    println("scope any d");

    println("-- move nocopy cast to any --");
    var moved_nocopy = MoveOnly{16};
    var e = (move moved_nocopy) as any;
    println("scope any e");

    println("-- same-type cast any --");
    var same_any_src: any = make_traced(18);
    var f = same_any_src as any;
    println("scope any f");
}

func test_coalesce_conversion_matrix() {
    println("=== Test 17: Coalesce conversion matrix ===");

    println("-- coalesce optional --");
    var none1: ?MoveOnly = null;
    var a = none1 ?? make_move_only(30);
    println("scope a");

    println("-- coalesce any --");
    var none2: ?MoveOnly = null;
    var b: any = none2 ?? make_move_only(40);
    println("scope b");
}

struct MoveHolder {
    value: MoveOnly;
}

struct MoveMaker {
    func make(id: int) MoveOnly {
        return make_move_only(id);
    }
}

struct TracedMaker {
    func make(id: int) Traced {
        return make_traced(id);
    }
}

func test_optional_chain_conversion_matrix() {
    println("=== Test 18: Optional-chain conversion matrix ===");

    println("-- optional chain field --");
    var h: ?MoveHolder = MoveHolder{value: make_move_only(50)};
    var a = h?.value;
    println("scope a");

    println("-- optional chain call --");
    var m: ?MoveMaker = MoveMaker{};
    var b = m?.make(60);
    println("scope b");

    println("-- optional chain call traced --");
    var tm: ?TracedMaker = TracedMaker{};
    var c = tm?.make(70);
    println("scope c");
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
    test_any_box_and_destroy();
    test_any_copy();
    test_any_reassign();
    test_any_move();
    test_any_string();
    test_any_int();
    test_any_conversion_matrix();
    test_any_cast_conversion_matrix();
    test_coalesce_conversion_matrix();
    test_optional_chain_conversion_matrix();
}
