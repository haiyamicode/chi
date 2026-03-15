import "std/ops" as ops;
import "./testdata/45_typedef/types" as types;
import {IntArray as IA, StringMap as SM} from "./testdata/45_typedef/types";

// --- Local generic struct for testing ---

struct Pair<A: ops.Construct, B: ops.Construct> {
    first: A = {};
    second: B = {};

    mut func new(first: A, second: B) {
        this.first = first;
        this.second = second;
    }
}

// --- Simple aliases ---
typedef IntArray = Array<int>;
typedef Str = string;
// --- Single-param generic typedef ---
typedef Span<T> = []T;
// --- Multi-param: full application ---
typedef IntPair = Pair<int, int>;
// --- Multi-param: partial application (pin first) ---
typedef IntFirst<B: ops.Construct> = Pair<int, B>;
// --- Multi-param: partial application (pin second) ---
typedef WithName<A: ops.Construct> = Pair<A, string>;
// --- Chained typedef: typedef of a generic typedef ---
typedef IntSpan = Span<int>;
// --- Partial application of stdlib generic ---
typedef StringMap<V> = Map<string, V>;
// --- Nested generics ---
typedef ArrayPair<T> = Pair<Array<T>, Array<T>>;
func sum(arr: IntArray) int {
    var total = 0;
    for item in arr {
        total = total + item;
    }
    return total;
}

func print_span(v: Span<int>) {
    for item in v {
        printf("{} ", item);
    }
    println("");
}

func format_int_pair(p: IntPair) string {
    return stringf("({}, {})", p.first, p.second);
}

func format_named<A>(p: WithName<A>) string {
    return stringf("{}: {}", p.second, p.first);
}

func main() {
    // Simple typedef alias
    var arr: IntArray = [1, 2, 3, 4, 5];
    printf("sum: {}\n", sum(arr));

    // Typedef is interchangeable with original type
    var arr2: Array<int> = arr;
    printf("arr2: {}\n", arr2);

    // String alias
    var s: Str = "world";
    printf("greeting: hello, {}\n", s);

    // Single-param generic typedef
    var v: Span<int> = arr.span();
    print_span(v);

    // Chained typedef (typedef of generic typedef)
    var iv: IntSpan = arr.span();
    printf("iv: {}\n", iv);

    // Interchangeable: IntSpan == Span<int> == []int
    var v2: []int = iv;
    var v3: Span<int> = v2;
    printf("v3: {}\n", v3);

    // Multi-param: full application (construct via typedef name)
    var p1 = IntPair{10, 20};
    printf("p1: {}\n", format_int_pair(p1));

    // IntPair is interchangeable with Pair<int, int>
    var p2: Pair<int, int> = p1;
    printf("p2: ({}, {})\n", p2.first, p2.second);

    // Multi-param: partial application (pin first)
    var p3: IntFirst<string> = Pair<int, string>{42, "answer"};
    printf("p3: {} = {}\n", p3.second, p3.first);

    // IntFirst<string> is interchangeable with Pair<int, string>
    var p4: Pair<int, string> = p3;
    printf("p4: {} = {}\n", p4.second, p4.first);

    // Multi-param: partial application (pin second)
    var p5: WithName<int> = Pair<int, string>{100, "score"};
    printf("named: {}\n", format_named<int>(p5));

    var p6: WithName<float> = Pair<float, string>{3.14, "pi"};
    printf("named: {}\n", format_named<float>(p6));

    // Partial application of Map (construct via typedef name)
    var m = StringMap<int>{};
    m.set("x", 10);
    m.set("y", 20);
    printf("m[x]: {}\n", m["x"]);
    printf("m[y]: {}\n", m["y"]);

    // StringMap<int> is interchangeable with Map<string, int>
    var m2: Map<string, int> = m;
    printf("m2[x]: {}\n", m2["x"]);

    // Nested generics
    var a1: Array<int> = [1, 2];
    var a2: Array<int> = [3, 4];
    var ap: ArrayPair<int> = Pair<Array<int>, Array<int>>{a1, a2};
    printf("ap.first: {}\n", ap.first);
    printf("ap.second: {}\n", ap.second);

    // ArrayPair<int> is interchangeable with Pair<Array<int>, Array<int>>
    var ap2: Pair<Array<int>, Array<int>> = ap;
    printf("ap2.first: {}\n", ap2.first);

    // Cross-module typedef (named import)
    var imported_arr: IA = [10, 20, 30];
    printf("imported: {}\n", imported_arr);

    // Cross-module generic typedef (named import with alias)
    var sm = SM<int>{};
    sm.set("z", 99);
    printf("sm[z]: {}\n", sm["z"]);
}
