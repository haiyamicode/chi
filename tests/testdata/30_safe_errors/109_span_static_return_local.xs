// Returning a local span as &'static [T] must be rejected.
// expect-error: does not live long enough

func bad() &'static [int] {
    var arr: Array<int> = [1, 2, 3];
    return arr.span();
}
