struct Pair<T> {
    a: ?T = null;
    b: ?T = null;
}

struct DeepWrap<T> {
    value: Pair<T>;

    static func make(v: T) DeepWrap<T> {
        return DeepWrap<T>{
            value: Pair<T>{ a: v },
        };
    }

    func wrap() DeepWrap<Pair<T>> {
        return DeepWrap<Pair<T>>{
            value: Pair<Pair<T>>{},
        };
    }
}

func main() {
    var d1 = DeepWrap<int>.make(42);
    var d2 = d1.wrap();
    var d3 = d2.wrap();
    var d4 = d3.wrap();
    var d5 = d4.wrap();
    var d6 = d5.wrap();
    var d7 = d6.wrap();
    var d8 = d7.wrap();
    var d9 = d8.wrap();
    var d10 = d9.wrap();

    printf("d1.a={}\n", d1.value.a!);
    printf("d10.a_is_null={}\n", d10.value.a == null);
    printf("d10.b_is_null={}\n", d10.value.b == null);

    var d10_copy = d10;
    printf("d10_copy.a_is_null={}\n", d10_copy.value.a == null);
}
