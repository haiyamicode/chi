// Reference type as generic type argument is rejected in safe mode.

func get_val<T>(val: T) T { return val; }

func dangle() &int {
    var local = 42;
    return get_val<&int>(&local);
}

func main() {
    printf("{}\n", dangle()!);
}
