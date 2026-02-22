// Calling an unsafe function without an unsafe block in safe mode
// expect-error: not allowed in safe mode
unsafe func dangerous() int {
    return 42;
}

func main() {
    var x = dangerous();  // error: call to unsafe function
}
