// Test: Tuple type support

// Basic tuple creation and field access
func test_basic() {
    var t = (1, 2, 3);
    println(t.0);
    println(t.1);
    println(t.2);
}

// Two-element tuple
func test_pair() {
    var pair = ("hello", 42);
    println(pair.0);
    println(pair.1);
}

// Tuple type annotation
func test_type_annotation() {
    var t: Tuple<int, string> = (10, "world");
    println(t.0);
    println(t.1);
}

// Tuple as function return
func get_pair() Tuple<int, string> {
    return (42, "answer");
}

func test_return() {
    var p = get_pair();
    println(p.0);
    println(p.1);
}

// Tuple as function parameter
func print_pair(p: Tuple<int, string>) {
    println(p.0);
    println(p.1);
}

func test_param() {
    print_pair((100, "param"));
}

// Nested tuples
func test_nested() {
    var t = ((1, 2), (3, 4));
    println(t.0.0);
    println(t.0.1);
    println(t.1.0);
    println(t.1.1);
}

// Mixed types
func test_mixed() {
    var t = (true, 3.14, "mixed", 99);
    println(t.0);
    println(t.1);
    println(t.2);
    println(t.3);
}

// Parenthesized expressions vs tuples — critical disambiguation
func test_paren_vs_tuple() {
    // Parenthesized expression — NOT a tuple
    var a = (5);
    println(a); // 5

    // Arithmetic in parentheses — NOT a tuple
    var b = (2 + 3);
    println(b); // 5

    // Tuple with arithmetic
    var c = (2 + 3, 4 * 5);
    println(c.0); // 5
    println(c.1); // 20

    // Nested parenthesized expressions inside tuple
    var d = ((2 + 3), (4 * 5));
    println(d.0); // 5
    println(d.1); // 20

    // Function call in parentheses — NOT a tuple
    var e = (get_five());
    println(e); // 5

    // Function call results in tuple
    var f = (get_five(), get_five() + 1);
    println(f.0); // 5
    println(f.1); // 6
}

func get_five() int {
    return 5;
}

// Unit value
func test_unit() {
    var u = ();
    println("unit ok");
}

// Tuple<> resolves to Unit
func test_tuple_unit() {
    var u: Tuple<> = ();
    println("tuple unit ok");
}

// Tuple in conditional and assignment
func test_conditional() {
    var t = (1, 2);
    var a = t.0;
    var b = t.1;
    if a + b == 3 {
        println("sum ok");
    }
}

// Tuple destructuring
func test_destructure() {
    var (a, b, c) = (1, 2, 3);
    println(a);
    println(b);
    println(c);
}

// Destructure from function return
func test_destructure_return() {
    var (n, s) = get_pair();
    println(n);
    println(s);
}

// Destructure nested tuple
func test_destructure_nested() {
    var (x, y) = ((10, 20), (30, 40));
    println(x.0);
    println(x.1);
    println(y.0);
    println(y.1);
}

// Rest destructuring
func test_destructure_rest() {
    var (a, ...rest) = (1, 2, 3);
    println(a);
    println(rest.0);
    println(rest.1);

    var (x, ...y) = (10, 20);
    println(x);
    println(y.0);

    var (head, ...tail) = ("hello", "world", "foo");
    println(head);
    println(tail.0);
    println(tail.1);
}

// AsTuple destructuring
import "std/ops" as ops;

struct Point {
    x: int;
    y: int;

    impl ops.AsTuple<int, int> {
        func as_tuple() Tuple<int, int> {
            return (this.x, this.y);
        }
    }
}

func test_as_tuple() {
    var p = Point{x: 10, y: 20};
    var (x, y) = p;
    println(x);
    println(y);

    // Rest with AsTuple
    var p2 = Point{x: 30, y: 40};
    var (a, ...rest) = p2;
    println(a);
    println(rest.0);
}

// Reassigning tuple fields through variable
func test_reassign() {
    var x = (10, 20);
    println(x.0);
    println(x.1);
    x = (30, 40);
    println(x.0);
    println(x.1);
}

// Regression: generic struct methods returning Tuple<T, bool> must specialize correctly.
struct AtomicLike<T> {
    value: T;

    static func from_value(value: T) AtomicLike<T> {
        return {:value};
    }

    func compare_exchange(expected: T) Tuple<T, bool> {
        return (expected, false);
    }
}

func test_generic_tuple_method_return() {
    var value = AtomicLike<int>.from_value(1);
    var (old, ok) = value.compare_exchange(7);
    println(old);
    println(ok);
}

func main() {
    test_basic();
    test_pair();
    test_type_annotation();
    test_return();
    test_param();
    test_nested();
    test_mixed();
    test_paren_vs_tuple();
    test_unit();
    test_tuple_unit();
    test_conditional();
    test_destructure();
    test_destructure_return();
    test_destructure_nested();
    test_destructure_rest();
    test_as_tuple();
    test_reassign();
    test_generic_tuple_method_return();
}
