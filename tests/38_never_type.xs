// === Custom never-returning function ===

func die(msg: string) never {
    panic(msg);
}

// === Guard clause narrowing with never ===

func guard_die(x: ?int) int {
    if !x {
        die("expected x");
    }
    return x + 1;
}

func guard_panic(x: ?int) int {
    if !x {
        panic("expected x");
    }
    return x + 1;
}

// === Positive narrowing with never in else ===

func positive_never(x: ?int) int {
    if x {
        return x + 1;
    } else {
        die("missing");
    }
}

// === Compound guard with never ===

func compound_guard(a: ?int, b: ?int) int {
    if !a || !b {
        die("both required");
    }
    return a + b;
}

// === Assert narrowing (assert calls panic which is never) ===

func assert_narrow(x: ?int) int {
    assert(x);
    return x + 1;
}

// === Never as bottom type in ?? ===

func coalesce(x: ?int) int {
    return x ?? die("no value");
}

// === Never in if-else type unification ===

func if_else_never(x: ?int) int {
    if x {
        return x;
    } else {
        die("missing");
    }
}

// === DotExpr narrowing with never guard ===

struct Wrapper {
    value: ?int = null;
}

func dot_guard_never(w: &Wrapper) int {
    if !w.value {
        die("no value");
    }
    return w.value + 1;
}

func main() {
    println("-- Guard clause narrowing --");
    printf("guard_die(5)={}\n", guard_die(5));
    printf("guard_panic(10)={}\n", guard_panic(10));

    println("\n-- Positive narrowing with never else --");
    printf("positive_never(3)={}\n", positive_never(3));

    println("\n-- Compound guard --");
    printf("compound_guard(1,2)={}\n", compound_guard(1, 2));

    println("\n-- Assert narrowing --");
    printf("assert_narrow(7)={}\n", assert_narrow(7));

    println("\n-- Never as bottom type --");
    printf("coalesce(42)={}\n", coalesce(42));
    printf("if_else_never(9)={}\n", if_else_never(9));

    println("\n-- DotExpr with never guard --");
    var w = Wrapper{};
    w.value = 100;
    printf("dot_guard_never={}\n", dot_guard_never(&w));
}
