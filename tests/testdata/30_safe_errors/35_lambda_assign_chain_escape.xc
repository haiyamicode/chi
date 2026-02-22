// Lambda escapes through an assignment chain:
// local -> lambda A -> var B = A -> return B
// expect-error: does not live long enough

func make_fn() func() int {
    var x = 42;
    var a = func() int { return x; };
    var b = a;
    return b;
}

func main() {
    printf("{}\n", make_fn()());
}
