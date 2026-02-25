func test_basic_rune() {
    var c: char = 'A';
    printf("char: {}\n", c);

    var r: rune = 'A';
    printf("rune ascii: {}\n", r);

    var r2 = '海';
    printf("rune unicode: {}\n", r2);

    var r3: rune = 'é';
    printf("rune escape: {}\n", r3);
}

func test_implicit_conversion() {
    var c: char = 'X';
    var r: rune = c;
    printf("char->rune: {}\n", r);

    var r2: rune = 'B';
    printf("literal->rune: {}\n", r2);
}

func test_explicit_conversion() {
    var r: rune = 'Z';
    var c: char = r as char;
    printf("rune->char: {}\n", c);

    var r2: rune = 65 as rune;
    printf("int->rune: {}\n", r2);

    var i: int = r2 as int;
    printf("rune->int: {}\n", i);
}

func test_arithmetic() {
    var r: rune = 'A';
    var r2 = r + 1;
    printf("A+1: {}\n", r2);

    var r3 = r2 - 1;
    printf("B-1: {}\n", r3);
}

func test_comparison() {
    var a: rune = 'A';
    var b: rune = 'B';
    printf("A<B: {}\n", a < b);
    printf("A==A: {}\n", a == a);
    printf("A!=B: {}\n", a != b);
}

func test_switch() {
    var r: rune = 'B';
    var result = switch r {
        'A' => "alpha",
        'B' => "bravo",
        else => "other"
    };
    printf("switch: {}\n", result);
}

func main() {
    test_basic_rune();
    test_implicit_conversion();
    test_explicit_conversion();
    test_arithmetic();
    test_comparison();
    test_switch();
}

