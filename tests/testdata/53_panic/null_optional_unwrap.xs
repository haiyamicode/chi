// expect-panic: panic: assertion failed: unwrapping null optional

struct Point {
    x: int = 0;
}

func main() {
    var p: ?Point = null;
    printf("before\n");
    printf("x={}\n", p!.x);
    printf("after\n");
}
