// Explicit deref through Shared<Iface> also yields a bare interface — must be rejected.
// expect-error: interface type 'Named' cannot be used directly
interface Named {
    func name() string;
}

struct Cat {
    impl Named { func name() string { return "cat"; } }
}

func main() {
    var c = Shared<Named>{new Cat{}};
    let msg = (*c).name();
    printf("{}\n", msg);
}
