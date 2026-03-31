// Test: Try blocks should be rejected with clear error

func main() {
    // Invalid: try block syntax
    try {
        println("hello");
    } catch {
        println("caught");
    };
}
