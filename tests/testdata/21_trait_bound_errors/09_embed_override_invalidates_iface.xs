// Wrapper embeds Dog (which implements Greet) but overrides greet() with a
// different signature. This invalidates Greet for Wrapper — assigning Wrapper
// to &Greet must be rejected.
// expect-error: cannot convert

interface Greet {
    func greet();
}

struct Dog {
    impl Greet {
        func greet() {
            printf("Woof!\n");
        }
    }
}

struct Wrapper {
    ...inner: Dog;

    func greet(extra: int) {
        printf("Wrapped: {}\n", extra);
    }
}

func main() {
    var w = Wrapper{inner: Dog{}};
    var g: &Greet = &w; // expect-error: cannot convert
}
