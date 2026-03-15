// Test: embedding a concrete generic type into a non-generic struct
struct IntStack {
    ...items: Array<int>;

    func peek() int {
        return this.items[this.items.length - 1];
    }

    // Uses promoted this.length directly (not this.items.length)
    func peek2() int {
        return this.items[this.length - 1];
    }
}

// Test: embedding a generic type into a generic struct (the main new feature)
struct Vec<T> {
    ...data: Array<T>;
}

// Test: method override — own add() caps at max_size using promoted this.length
struct FixedVec<T> {
    ...data: Array<T>;
    max_size: uint32;

    func push(item: T) {
        if this.length < this.max_size {
            this.data.push(item);
        }
    }
}

// Test: field override — own field shadows promoted field from embed
struct CountedVec<T> {
    ...data: Array<T>;
    length: uint32;
}

// Test: chained generic embedding — Stack<T> wraps Array<T>, DoubleStack<T> wraps Stack<T>
struct Stack<T> {
    ...data: Array<T>;
}

struct DoubleStack<T> {
    ...inner: Stack<T>;
}

// Test: multi-type-param with two independent embeds
struct Pair<K, V> {
    ...keys: Array<K>;
    ...values: Array<V>;
}

// Test: override a promoted method with a different signature — allowed when the
// embedded struct does NOT implement any interface requiring that method.
struct Barker {
    func greet() {
        printf("Woof!\n");
    }
}

struct SilentWrapper {
    ...inner: Barker;

    // Different signature than Barker.greet — allowed, no interface involved
    func greet(msg: string) {
        printf("Silent: {}\n", msg);
    }
}

// Test: override a promoted interface method with a different signature — allowed.
// The embedded Dog implements Greet; Wrapper's override invalidates that interface
// for Wrapper (Wrapper no longer satisfies Greet), but compiles fine.
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
    // --- Non-generic struct embedding concrete generic ---
    var s = IntStack{items: Array<int>{}};
    s.push(10);
    s.push(20);
    s.push(30);
    printf("IntStack length: {}\n", s.length);
    printf("IntStack peek: {}\n", s.peek());
    printf("IntStack peek2: {}\n", s.peek2());

    s.clear();
    printf("IntStack after clear: {}\n", s.length);

    // --- Generic struct embedding generic ---
    var vi = Vec<int>{data: Array<int>{}};
    vi.push(1);
    vi.push(2);
    vi.push(3);
    printf("Vec<int> length: {}\n", vi.length);

    var vs = Vec<string>{data: Array<string>{}};
    vs.push("hello");
    vs.push("world");
    printf("Vec<string> length: {}\n", vs.length);

    vs.clear();
    printf("Vec<string> after clear: {}\n", vs.length);

    // --- Method override: own add() uses promoted this.length to cap at max_size ---
    var fv = FixedVec<int>{data: Array<int>{}, max_size: 2};
    fv.push(1);
    fv.push(2);
    fv.push(3); // capped
    printf("FixedVec length: {}\n", fv.length);
    printf("FixedVec max_size: {}\n", fv.max_size);

    // --- Field override: own length field shadows promoted one ---
    var cv = CountedVec<int>{data: Array<int>{}, length: 99};
    cv.data.push(1);
    cv.data.push(2);
    printf("CountedVec data.length: {}\n", cv.data.length);
    printf("CountedVec own length: {}\n", cv.length);

    // --- Chained generic embedding ---
    var ds = DoubleStack<int>{inner: Stack<int>{data: Array<int>{}}};
    ds.push(1);
    ds.push(2);
    ds.push(3);
    printf("DoubleStack length: {}\n", ds.length);
    ds.clear();
    printf("DoubleStack after clear: {}\n", ds.length);

    // --- Multi-type-param with two embeds ---
    var p = Pair<int, string>{keys: Array<int>{}, values: Array<string>{}};
    p.keys.push(1);
    p.keys.push(2);
    p.values.push("a");
    printf("Pair keys length: {}\n", p.keys.length);
    printf("Pair values length: {}\n", p.values.length);

    // --- Override with different signature when no interface is involved ---
    var sw = SilentWrapper{inner: Barker{}};
    sw.greet("hello");

    // --- Override invalidates embedded interface ---
    var w = Wrapper{inner: Dog{}};
    w.greet(42);
}
