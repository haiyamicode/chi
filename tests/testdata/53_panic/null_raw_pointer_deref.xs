// expect-panic: panic: null pointer dereference

func main() {
    var p: *int = null;
    unsafe {
        printf("{}\n", *p);
    }
}
