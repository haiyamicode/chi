// Using = {} on placeholder type without Construct bound — must be rejected
// expect-error: cannot default-construct
struct MissingBoundBox<T> {
    item: T = {};
}

func main() {
    var b = MissingBoundBox<int>{};
}
