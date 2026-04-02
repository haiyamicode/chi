// expect-panic: panic: null pointer dereference
// expect-output: in function bar
// expect-output: in function baz
// expect-output: in function main

func bar(p: *int) int {
    unsafe {
        return *p;
    }
}

func baz() {
    var p: *int = null;
    let val = bar(p);
    printf("val={}\n", val);
}

func main() {
    baz();
}
