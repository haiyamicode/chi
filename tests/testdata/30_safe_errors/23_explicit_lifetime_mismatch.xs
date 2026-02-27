// Returning param with wrong explicit lifetime.
// Return type has 'a but b has 'b — no bound, can't satisfy.
// expect-error: does not live long enough

func bad<'a, 'b>(a: &'a int, b: &'b int) &'a int {
    return b;
}

func main() {
    var x = 10;
    var y = 20;
    var r = bad(&x, &y);
}

