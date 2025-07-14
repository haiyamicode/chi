// Test case for potential binary expression infinite loop
func main() {
    var x = 1 @@ 2;  // Invalid operator should not cause infinite loop
}