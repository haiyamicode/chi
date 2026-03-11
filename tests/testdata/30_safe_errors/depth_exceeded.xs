// expect-error: exceeds maximum nesting depth

struct W<T> { value: ?T = null; }

struct MyBox<T> {
    value: ?T = null;
    func wrap() MyBox<W<T>> { return MyBox<W<T>>{}; }
}

func main() {
    var b = MyBox<int>{};
    var w1 = b.wrap();
    var w2 = w1.wrap();
    var w3 = w2.wrap();
    var w4 = w3.wrap();
    var w5 = w4.wrap();
    var w6 = w5.wrap();
    var w7 = w6.wrap();
    var w8 = w7.wrap();
    var w9 = w8.wrap();
    var w10 = w9.wrap();
    var w11 = w10.wrap();
    var w12 = w11.wrap();
    var w13 = w12.wrap();
    var w14 = w13.wrap();
    var w15 = w14.wrap();
    var w16 = w15.wrap();
    var w17 = w16.wrap();
}
