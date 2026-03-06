import "std/ops" as ops;

struct Point {
    x: int = 0;
    y: int = 0;
    name: string = "";
}

struct Traced {
    id: int = 0;

    mut func new(id: int = 0) {
        this.id = id;
        if id != 0 {
            printf("Traced({}) created\n", id);
        }
    }

    mut func delete() {
        if this.id != 0 {
            printf("Traced({}) destroyed\n", this.id);
        }
    }

    impl ops.CopyFrom<Traced> {
        mut func copy_from(source: &Traced) {
            this.id = source.id;
            if source.id != 0 {
                printf("Traced({}) copied\n", source.id);
            }
        }
    }
}

func test_basic() {
    println("=== basic ===");
    var a = [3]int{10, 20, 30};
    println(a[0]);
    println(a[1]);
    println(a[2]);
    println(a.length);
}

func test_mutation() {
    println("=== mutation ===");
    var a = [3]int{10, 20, 30};
    a[1] = 42;
    println(a[1]);
}

func test_literal_syntax() {
    println("=== literal syntax ===");
    var b: [3]int = [1, 2, 3];
    println(b[0]);
    println(b[1]);
    println(b[2]);
}

func test_for_in() {
    println("=== for-in ===");
    var a = [3]int{10, 20, 30};
    for item in a {
        println(item);
    }
}

func test_for_in_index() {
    println("=== for-in with index ===");
    var b: [3]int = [1, 2, 3];
    for item, i in b {
        printf("{}: {}\n", i, item);
    }
}

func test_ref_bind() {
    println("=== ref bind ===");
    var a = [3]int{10, 20, 30};
    for &mut item in a {
        *item = *item + 100;
    }
    for item in a {
        println(item);
    }
}

func test_strings() {
    println("=== strings ===");
    var names = [3]string{"hello", "world", "chi"};
    for name in names {
        println(name);
    }
}

func test_copy() {
    println("=== copy ===");
    var names = [3]string{"hello", "world", "chi"};
    var c = names;
    c[0] = "changed";
    println(c[0]);
    println(names[0]);
}

func test_partial_init() {
    println("=== partial init ===");
    var d = [5]int{1, 2};
    for item in d {
        println(item);
    }
}

func test_struct_array() {
    println("=== struct array ===");
    var points = [2]Point{
        Point{
            x: 1,
            y: 2,
            name: "a"
        },
        Point{
            x: 3,
            y: 4,
            name: "b"
        }
    };
    for p in points {
        printf("{} ({}, {})\n", p.name, p.x, p.y);
    }
}

func test_struct_copy() {
    println("=== struct copy ===");
    var points = [2]Point{
        Point{
            x: 1,
            y: 2,
            name: "a"
        },
        Point{
            x: 3,
            y: 4,
            name: "b"
        }
    };
    var points2 = points;
    points2[0].name = "modified";
    println(points[0].name);
    println(points2[0].name);
}

func test_lifecycle_construct_destroy() {
    println("=== lifecycle: construct + destroy ===");
    var arr = [3]Traced{Traced{1}, Traced{2}, Traced{3}};
    println("before scope exit");
}

func test_lifecycle_partial_init() {
    println("=== lifecycle: partial init ===");
    var arr = [3]Traced{Traced{10}};
    println("before scope exit");
}

func test_lifecycle_copy() {
    println("=== lifecycle: copy ===");
    var arr = [3]Traced{Traced{1}, Traced{2}, Traced{3}};
    println("copying...");
    var arr2 = arr;
    arr2[0].id = 99;
    printf("original[0] = {}\n", arr[0].id);
    printf("copy[0] = {}\n", arr2[0].id);
    println("before scope exit");
}

func test_lifecycle_for_in_value() {
    println("=== lifecycle: for-in value copy ===");
    var arr = [2]Traced{Traced{1}, Traced{2}};
    println("iterating...");
    for item in arr {
        printf("item: {}\n", item.id);
    }
    println("before scope exit");
}

func print_arr(arr: &[3]int) {
    for item in arr {
        println(item);
    }
}

func double_arr(arr: &mut [3]int) {
    for &mut item in arr {
        *item = *item * 2;
    }
}

func test_ref_iteration() {
    println("=== ref iteration ===");
    var a = [3]int{10, 20, 30};
    print_arr(&a);
    double_arr(&mut a);
    print_arr(&a);
}

func test_zeroinit() {
    println("=== zeroinit ===");
    var a: [5]int = zeroinit;
    for item in a {
        println(item);
    }
    // struct zeroinit skips constructor (default x=0, y=0, not field defaults)
    var p: Point = zeroinit;
    printf("{} {} {}\n", p.x, p.y, p.name);
}

func main() {
    test_basic();
    test_mutation();
    test_literal_syntax();
    test_for_in();
    test_for_in_index();
    test_ref_bind();
    test_strings();
    test_copy();
    test_partial_init();
    test_struct_array();
    test_struct_copy();
    test_lifecycle_construct_destroy();
    test_lifecycle_partial_init();
    test_lifecycle_copy();
    test_lifecycle_for_in_value();
    test_ref_iteration();
    test_zeroinit();
}

