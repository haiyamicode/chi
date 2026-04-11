// Test map initialization syntax with KvInit interface

import "std/ops" as ops;

struct Point {
    x: int;
    y: int;

    impl ops.Copy {
        mut func copy(source: &This) {
            this.x = source.x;
            this.y = source.y;
        }
    }
}

func main() {
    // Explicit type with multiple entries
    println("=== explicit ===");
    var m1 = Map<string, int>{
        "hello": 1,
        "world": 2,
        "chi": 3
    };
    if let v = m1.get("hello") {
        printf("hello = {}\n", *v);
    }
    if let v = m1.get("world") {
        printf("world = {}\n", *v);
    }
    if let v = m1.get("chi") {
        printf("chi = {}\n", *v);
    }
    if let v = m1.get("missing") {
        printf("missing = {}\n", *v);
    } else {
        println("missing not found");
    }

    // Contextual type inference
    println("=== contextual ===");
    var m2 = Map<string, int>{"foo": 10, "bar": 20};
    if let v = m2.get("foo") {
        printf("foo = {}\n", *v);
    }
    if let v = m2.get("bar") {
        printf("bar = {}\n", *v);
    }

    // Heap allocated
    println("=== new ===");
    var m3 = new Map<string, int>{"x": 100};
    if let v = m3.get("x") {
        printf("x = {}\n", *v);
    }

    // String values
    println("=== string values ===");
    var m4 = Map<string, string>{"lang": "chi", "type": "compiled"};
    if let v = m4.get("lang") {
        printf("lang = {}\n", *v);
    }
    if let v = m4.get("type") {
        printf("type = {}\n", *v);
    }

    // Single entry
    println("=== single ===");
    var m5 = Map<string, int>{"only": 1};
    if let v = m5.get("only") {
        printf("only = {}\n", *v);
    }

    // Int keys
    println("=== int keys ===");
    var m6 = Map<int, string>{
        1: "one",
        2: "two",
        3: "three"
    };
    if let v = m6.get(1) {
        printf("1 = {}\n", *v);
    }
    if let v = m6.get(2) {
        printf("2 = {}\n", *v);
    }
    if let v = m6.get(3) {
        printf("3 = {}\n", *v);
    }

    // Nested: Map of arrays
    println("=== nested array values ===");
    var m7 = Map<string, Array<int>>{"nums": [1, 2, 3], "more": [4, 5]};
    if let v = m7.get("nums") {
        printf("nums len = {}\n", v.length);
    }
    if let v = m7.get("more") {
        printf("more len = {}\n", v.length);
    }

    // Struct values
    println("=== struct values ===");
    var m8 = Map<string, Point>{"origin": {x: 0, y: 0}, "offset": {x: 10, y: 20}};
    if let v = m8.get("origin") {
        printf("origin = ({}, {})\n", v.x, v.y);
    }
    if let v = m8.get("offset") {
        printf("offset = ({}, {})\n", v.x, v.y);
    }

    // Int key with struct value
    println("=== int key struct value ===");
    var m9 = Map<int, Point>{0: {x: 1, y: 2}, 1: {x: 3, y: 4}};
    if let v = m9.get(0) {
        printf("0 = ({}, {})\n", v.x, v.y);
    }
    if let v = m9.get(1) {
        printf("1 = ({}, {})\n", v.x, v.y);
    }
}
