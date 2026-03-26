// Reference type as generic type argument is allowed, but dangling return must still be rejected.
// expect-error: does not live long enough

func get_val<T>(val: T) T {
    return val;
}

func dangle() &int {
    var local = 42;
    return get_val<&int>(&local);
}

func main() {
    printf("{}\n", dangle()!);
}
