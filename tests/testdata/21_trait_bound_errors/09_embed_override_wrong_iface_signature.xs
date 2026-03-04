// Embedding a struct that implements an interface, then overriding the
// interface method with a non-compliant signature — must be rejected.
// expect-error: does not match definition from interface
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

    // Wrong signature: Greet requires greet(), but user adds a parameter
    func greet(extra: int) {
        printf("Wrapped: {}\n", extra);
    }
}

func main() {
    var w = Wrapper{ inner: Dog{} };
    w.greet(42);
}
