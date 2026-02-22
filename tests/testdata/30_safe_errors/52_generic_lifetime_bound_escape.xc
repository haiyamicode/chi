// Generic with lifetime bound still catches dangling references at call site.
// expect-error: does not live long enough

func get_val<'a, T: 'a>(val: T) T { return val; }

func dangle() &int {
    var local = 42;
    return get_val<&int>(&local);
}

func main() {
    printf("{}\n", dangle()!);
}
