#!/bin/bash
# Fuzz the analyzer with partial typing patterns to find crashes
# Each test simulates what the user's editor sends mid-keystroke

CHIC=../../build/bin/chic
TMPFILE=/tmp/chi_fuzz_test.xs
PASSED=0
FAILED=0
CRASHES=""

test_pattern() {
    local name="$1"
    local code="$2"
    printf "Testing %-55s ... " "$name"
    echo "$code" > "$TMPFILE"
    output=$(timeout 5s env TIMES=1 $CHIC --analyzer -c "$TMPFILE" 2>&1)
    exit_code=$?
    if [ $exit_code -eq 124 ]; then
        echo "TIMEOUT (infinite loop)"
        FAILED=$((FAILED + 1))
        CRASHES="$CRASHES\n  TIMEOUT: $name"
    elif [ $exit_code -ge 128 ]; then
        sig=$((exit_code - 128))
        echo "CRASH (signal $sig)"
        FAILED=$((FAILED + 1))
        CRASHES="$CRASHES\n  CRASH(sig$sig): $name"
    else
        echo "OK"
        PASSED=$((PASSED + 1))
    fi
}

echo "=== Chi Analyzer Partial Typing Fuzzer ==="
echo ""

# --- Bare keywords ---
echo "--- Bare keywords in function body ---"
for kw in func var struct enum if for while return switch import defer delete sizeof; do
    test_pattern "bare '$kw'" "func main() { $kw }"
    test_pattern "bare '$kw;'" "func main() { $kw; }"
done

# --- Incomplete lambda patterns ---
echo ""
echo "--- Incomplete lambda patterns ---"
test_pattern "func(" "func main() { var f = func( }"
test_pattern "func()" "func main() { var f = func() }"
test_pattern "func() {" "func main() { var f = func() { }"
test_pattern "func() =>" "func main() { var f = func() => }"
test_pattern "func(x" "func main() { var f = func(x }"
test_pattern "func(x," "func main() { var f = func(x, }"
test_pattern "func(x, y" "func main() { var f = func(x, y }"
test_pattern "func(x) =>" "func main() { var f = func(x) => }"
test_pattern "func(x) => x" "func main() { var f = func(x) => x }"
test_pattern "func(x) => x +" "func main() { var f = func(x) => x + }"
test_pattern "func(x: int" "func main() { var f = func(x: int }"
test_pattern "func(x: int)" "func main() { var f = func(x: int) }"
test_pattern "func(x: int) =>" "func main() { var f = func(x: int) => }"
test_pattern "func(x: int) int" "func main() { var f = func(x: int) int }"
test_pattern "func(x: int) int {" "func main() { var f = func(x: int) int { }"
test_pattern "func as call arg" "func main() { foo(func) }"
test_pattern "func() as call arg" "func main() { foo(func()) }"
test_pattern "func(x) => as arg" "func main() { foo(func(x) =>) }"
test_pattern "func in chain" "func main() { a.b(func) }"
test_pattern "func => in chain" "func main() { a.b(func(x) => x).c(func) }"

# --- Incomplete dot expressions ---
echo ""
echo "--- Incomplete dot expressions ---"
test_pattern "x." "func main() { var x = 1; x. }"
test_pattern "x.y." "func main() { var x = 1; x.y. }"
test_pattern "x.y.z." "func main() { var x = 1; x.y.z. }"
test_pattern "\"str\"." "func main() { \"hello\". }"
test_pattern "array." "func main() { var a: Array<int> = {}; a. }"
test_pattern "chain.method." "func main() { var a: Array<int> = {}; a.filter(func(x) => true). }"
test_pattern "chain.method.method." "func main() { var a: Array<int> = {}; a.filter(func(x) => true).map(func(x) => x). }"

# --- Incomplete var declarations ---
echo ""
echo "--- Incomplete var declarations ---"
test_pattern "var" "func main() { var }"
test_pattern "var x" "func main() { var x }"
test_pattern "var x:" "func main() { var x: }"
test_pattern "var x: int" "func main() { var x: int }"
test_pattern "var x: int =" "func main() { var x: int = }"
test_pattern "var x =" "func main() { var x = }"
test_pattern "var x: Array<" "func main() { var x: Array< }"
test_pattern "var x: Array<int" "func main() { var x: Array<int }"
test_pattern "var x: Array<int>" "func main() { var x: Array<int> }"
test_pattern "var x: func" "func main() { var x: func }"
test_pattern "var x: func (" "func main() { var x: func ( }"
test_pattern "var x: func (int" "func main() { var x: func (int }"
test_pattern "var x: func (int)" "func main() { var x: func (int) }"
test_pattern "var x: func (int) int" "func main() { var x: func (int) int }"

# --- Incomplete struct declarations ---
echo ""
echo "--- Incomplete struct declarations ---"
test_pattern "struct" "struct"
test_pattern "struct Foo" "struct Foo"
test_pattern "struct Foo {" "struct Foo {"
test_pattern "struct Foo { x" "struct Foo { x }"
test_pattern "struct Foo { x:" "struct Foo { x: }"
test_pattern "struct Foo { x: int" "struct Foo { x: int }"
test_pattern "struct Foo { func" "struct Foo { func }"
test_pattern "struct Foo { func f" "struct Foo { func f }"
test_pattern "struct Foo { func f(" "struct Foo { func f( }"
test_pattern "struct Foo<" "struct Foo< {"
test_pattern "struct Foo<T" "struct Foo<T {"
test_pattern "struct Foo<T>" "struct Foo<T> {"
test_pattern "struct implements" "struct Foo implements {"
test_pattern "struct impl name" "struct Foo implements Bar {"

# --- Incomplete if/for/while ---
echo ""
echo "--- Incomplete control flow ---"
test_pattern "if" "func main() { if }"
test_pattern "if (" "func main() { if ( }"
test_pattern "if (true" "func main() { if (true }"
test_pattern "if true" "func main() { if true }"
test_pattern "if true {" "func main() { if true { }"
test_pattern "if x ==" "func main() { var x = 1; if x == }"
test_pattern "for" "func main() { for }"
test_pattern "for x" "func main() { for x }"
test_pattern "for x in" "func main() { for x in }"
test_pattern "for x in arr" "func main() { var arr: Array<int> = {}; for x in arr }"
test_pattern "while" "func main() { while }"
test_pattern "while true" "func main() { while true }"
test_pattern "while (" "func main() { while ( }"

# --- Incomplete function declarations ---
echo ""
echo "--- Incomplete function declarations ---"
test_pattern "func" "func"
test_pattern "func foo" "func foo"
test_pattern "func foo(" "func foo("
test_pattern "func foo(x" "func foo(x"
test_pattern "func foo(x:" "func foo(x:"
test_pattern "func foo(x: int" "func foo(x: int"
test_pattern "func foo(x: int)" "func foo(x: int)"
test_pattern "func foo(x: int) int" "func foo(x: int) int"
test_pattern "func foo(x: int) int {" "func foo(x: int) int {"
test_pattern "func foo<" "func foo<"
test_pattern "func foo<T" "func foo<T"
test_pattern "func foo<T>" "func foo<T>("
test_pattern "func foo<T>(x: T" "func foo<T>(x: T"

# --- Incomplete enum ---
echo ""
echo "--- Incomplete enum ---"
test_pattern "enum" "enum"
test_pattern "enum Foo" "enum Foo"
test_pattern "enum Foo {" "enum Foo {"
test_pattern "enum Foo { A" "enum Foo { A }"
test_pattern "enum Foo { A," "enum Foo { A, }"
test_pattern "enum Foo { A(int" "enum Foo { A(int }"

# --- Incomplete expressions ---
echo ""
echo "--- Incomplete expressions ---"
test_pattern "binary +" "func main() { var x = 1 + }"
test_pattern "binary *" "func main() { var x = 1 * }"
test_pattern "binary ==" "func main() { var x = 1 == }"
test_pattern "binary &&" "func main() { var x = true && }"
test_pattern "binary ||" "func main() { var x = true || }"
test_pattern "unary !" "func main() { var x = ! }"
test_pattern "unary -" "func main() { var x = - }"
test_pattern "cast as" "func main() { var x = 1 as }"
test_pattern "cast as type" "func main() { var x = 1 as char }"
test_pattern "array literal {" "func main() { var x: Array<int> = { }"
test_pattern "array literal {1," "func main() { var x: Array<int> = {1, }"
test_pattern "array literal {1, 2" "func main() { var x: Array<int> = {1, 2 }"
test_pattern "index []" "func main() { var a: Array<int> = {}; a[ }"
test_pattern "index [0" "func main() { var a: Array<int> = {}; a[0 }"
test_pattern "fn call (" "func main() { printf( }"
test_pattern "fn call (x," "func main() { printf(x, }"
test_pattern "method call ." "func main() { var a: Array<int> = {}; a.filter( }"
test_pattern "method chained" "func main() { var a: Array<int> = {}; a.filter(func(n) => true).map( }"
test_pattern "nested parens" "func main() { var x = ((( }"
test_pattern "string interpolation" 'func main() { var x = "hello {}"; }'

# --- Incomplete return ---
echo ""
echo "--- Incomplete return ---"
test_pattern "return" "func main() { return }"
test_pattern "return expr" "func main() int { return 1 + }"
test_pattern "return func" "func main() { return func }"
test_pattern "return func()" "func main() { return func() }"

# --- Incomplete switch ---
echo ""
echo "--- Incomplete switch ---"
test_pattern "switch" "func main() { switch }"
test_pattern "switch x" "func main() { var x = 1; switch x }"
test_pattern "switch x {" "func main() { var x = 1; switch x { }"
test_pattern "switch case" "func main() { var x = 1; switch x { case }"
test_pattern "switch case 1" "func main() { var x = 1; switch x { case 1 }"
test_pattern "switch case 1:" "func main() { var x = 1; switch x { case 1: }"

# --- Nested incomplete lambdas (the original bug pattern) ---
echo ""
echo "--- Nested/chained lambda patterns ---"
test_pattern "chained filter.map(func)" \
    "func main() { var a: Array<int> = {1,2,3}; a.filter(func(n) => true).map(func) }"
test_pattern "chained filter.map(func()" \
    "func main() { var a: Array<int> = {1,2,3}; a.filter(func(n) => true).map(func()) }"
test_pattern "chained filter.map(func(x)" \
    "func main() { var a: Array<int> = {1,2,3}; a.filter(func(n) => true).map(func(x)) }"
test_pattern "chained filter.map(func(x) =>" \
    "func main() { var a: Array<int> = {1,2,3}; a.filter(func(n) => true).map(func(x) =>) }"
test_pattern "chained filter.map(func(x) => x" \
    "func main() { var a: Array<int> = {1,2,3}; a.filter(func(n) => true).map(func(x) => x) }"
test_pattern "chained filter.map(func(x) => x +" \
    "func main() { var a: Array<int> = {1,2,3}; a.filter(func(n) => true).map(func(x) => x +) }"
test_pattern "nested lambda in lambda" \
    "func main() { var f = func(x) { return func(y) => }; }"
test_pattern "lambda with capture [" \
    "func main() { var x = 1; var f = func [x }"
test_pattern "lambda with capture [x]" \
    "func main() { var x = 1; var f = func [x] }"
test_pattern "lambda with capture [x](" \
    "func main() { var x = 1; var f = func [x]( }"
test_pattern "lambda with capture [x](y) =>" \
    "func main() { var x = 1; var f = func [x](y) => }"

# --- Import patterns ---
echo ""
echo "--- Incomplete imports ---"
test_pattern "import" "import"
test_pattern "import \"" "import \""
test_pattern "import \"foo" "import \"foo"

# --- Multi-line realistic typing scenarios ---
echo ""
echo "--- Realistic mid-typing scenarios ---"
test_pattern "typing struct method" \
    "struct Foo { x: int; func bar(self) { self. } }"
test_pattern "typing after struct field" \
    "struct Foo { x: int; y: }"
test_pattern "typing generic struct" \
    "struct Foo<T> { x: T; func bar(self) T { return self. } }"
test_pattern "typing interface impl" \
    "struct Foo implements { }"
test_pattern "var with struct init" \
    "struct Foo { x: int; } func main() { var f: Foo = { }"
test_pattern "partial method chain on string" \
    'func main() { var s = "hello"; s. }'
test_pattern "empty func body in struct" \
    "struct Foo { func bar() { } func baz() }"
test_pattern "multiple incomplete stmts" \
    "func main() { var x = ; var y = ; }"
test_pattern "lambda returning lambda" \
    "func main() { var f: func() func() int = func() => func() => }"

echo ""
echo "=== Results: $PASSED OK, $FAILED FAILED ==="
if [ -n "$CRASHES" ]; then
    echo -e "Failures:$CRASHES"
fi

rm -f "$TMPFILE"
exit $FAILED
