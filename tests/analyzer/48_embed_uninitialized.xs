// Embedded struct fields must be initialized like regular fields.

struct Inner {
    x: int = 0;
    y: int = 0;
}

// Error: embed not initialized in constructor
struct Bad1 {
    ...inner: Inner;
    mut func new() {}
}

// Error: embed not initialized, no default expression
struct Bad2 {
    ...inner: Inner;
    z: int = 0;
    mut func new() {
        this.z = 1;
    }
}

struct RequiredFields {
    x: int;
    mut func new(v: int) {
        this.x = v;
    }
}

// Error: embed with required fields not initialized
struct Bad3 {
    ...inner: RequiredFields;
    mut func new() {}
}

func main() {}
