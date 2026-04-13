struct Pair<T> {
    a: ?T = null;
    b: ?T = null;
}

struct DeepWrap<T> {
    value: Pair<T>;

    static func make(v: T) DeepWrap<T> {
        return {value: {a: v}};
    }

    func wrap() DeepWrap<Pair<T>> {
        return {value: {}};
    }
}

// Nested generic struct with Shared<Inner<T>> and auto-deref method dispatch.
// Exercises: compile_fn_proto Struct container type_env recovery,
// resolve_variant_type_id ensuring variants are registered,
// and on-demand compile in get_fn for substituted container methods.
struct Wrap<T> {
    value: T;
}

func make_wrap<T>(v: T) Wrap<T> {
    return {value: v};
}

func recur<T>(depth: int, v: T) {
    if depth <= 0 {
        return;
    }
    recur<Wrap<T>>(depth - 1, make_wrap(v));
}

struct State<T> {
    value: T;
    count: int = 0;
}

struct Holder<T> {
    data: Shared<State<T>>;

    mut func new(value: T) {
        this.data = {new {:value}};
    }

    mut func increment() {
        this.data.mut().count += 1;
    }

    func count() int {
        return this.data.count;
    }
}

func main() {
    recur<int>(3, 0);
    printf("recur done\n");

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

    // Nested generic: Shared<State<T>> inside Holder<T>
    // Exercises auto-deref through Shared<State<T>> for field access and method dispatch
    var h1 = Holder<int>{10};
    printf("h1.value={}\n", h1.data.value);
    h1.increment();
    printf("h1.count={}\n", h1.count());

    var h2 = Holder<string>{"hello"};
    printf("h2.value={}\n", h2.data.value);
    h2.increment();
    h2.increment();
    printf("h2.count={}\n", h2.count());

    // Nested with another generic: Shared<State<Pair<T>>>
    var h3 = Holder<Pair<int>>{{a: 1, b: 2}};
    printf("h3.value.a={}\n", h3.data.value.a!);
    printf("h3.value.b={}\n", h3.data.value.b!);
}
