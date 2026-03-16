// Void should be rejected by the implicit Sized bound on unbounded type params
// expect-error: does not satisfy trait bound 'Sized'
struct VoidArgBox<T> {}

func main() {
    var b = VoidArgBox<void>{};
    let _ = b;
}
