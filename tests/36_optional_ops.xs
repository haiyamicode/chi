struct Point {
    x: int = 0;
    y: int = 0;

    func sum() int {
        return this.x + this.y;
    }
}

func test_null_coalescing() {
    println("test_null_coalescing:");

    var a: ?int = 42;
    var result = a ?? 0;
    printf("a ?? 0 = {}\n", result);

    var b: ?int = null;
    result = b ?? 99;
    printf("b ?? 99 = {}\n", result);

    println("");
}

func test_optional_chain_field() {
    println("test_optional_chain_field:");

    var p: ?Point = Point{x: 10, y: 20};
    var x = p?.x;
    printf("p?.x = {}\n", x);

    var q: ?Point = null;
    var y = q?.y;
    printf("q?.y = {}\n", y);

    println("");
}

func test_optional_chain_method() {
    println("test_optional_chain_method:");

    var p: ?Point = Point{x: 3, y: 4};
    var s = p?.sum();
    printf("p?.sum() = {}\n", s);

    var q: ?Point = null;
    var s2 = q?.sum();
    printf("q?.sum() = {}\n", s2);

    println("");
}

func test_chaining() {
    println("test_chaining:");

    var p: ?Point = Point{x: 100, y: 200};
    var x = p?.x ?? -1;
    printf("p?.x ?? -1 = {}\n", x);

    var q: ?Point = null;
    var y = q?.y ?? -1;
    printf("q?.y ?? -1 = {}\n", y);

    println("");
}

func main() {
    test_null_coalescing();
    test_optional_chain_field();
    test_optional_chain_method();
    test_chaining();
}

