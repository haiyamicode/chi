// Two lambdas where the second captures a local declared after the first,
// then the first is reassigned to call the second — LIFO violation.

func main() {
    var f1: func() int = func() int { return 0; };
    var val = 100;
    var f2 = func() int { return val; };
    f1 = func() int { return f2(); };
    printf("{}\n", f1());
}
