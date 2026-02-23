// Return through a pick function where one argument is a dangling local.
// expect-error: does not live long enough

func pick(a: &int, b: &int, cond: bool) &int {
    if cond {
        return a;
    }
    return b;
}

func get_dangling(x: &int) &int {
    var local = 999;
    return pick(x, &local, false);
}

func main() {
    var safe = 1;
    printf("{}\n", get_dangling(&safe)!);
}

