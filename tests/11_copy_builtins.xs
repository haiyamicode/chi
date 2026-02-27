func test_string_pass(str: string) {
    printf("passed string: {}\n");
}

func test_string() {
    var s = string.format("hello {}", "world");
    var t = s;
    println(t);
    test_string_pass(s);
}

func test_array() {
    var a: Array<int> = [];
    a.add(1);
    a.add(2);
    var b = a;
    a.clear();
    a.add(3);
    printf("a.length: {}\n", a.length);
    printf("b.length: {}\n", b.length);
    printf("a[0]: {}\n", a[0]);
    printf("b[0]: {}\n", b[0]);
}

func main() {
    test_string();
    test_array();
}

