// Deeply nested incomplete structures
struct Outer {
    struct Inner {
        enum State {
            A {
                var field: func(int) -> {
                    struct VeryNested {
                        var x