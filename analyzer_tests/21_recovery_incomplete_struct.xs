// Test error recovery for incomplete struct declaration
struct MyStruct
    // Missing opening brace and body

func main() {
    var x = 42;
    // Should still parse this function correctly despite the broken struct above
    print("Hello");
}