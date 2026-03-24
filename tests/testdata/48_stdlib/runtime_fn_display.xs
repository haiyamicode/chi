func foo() int {
    return 1;
}

struct S {
    func bar() int {
        return 2;
    }
}

func main() {
    println(foo);

    let s = S{};
    println(s.bar);

    let f = func () int {
        return 3;
    };
    println(f);
}
