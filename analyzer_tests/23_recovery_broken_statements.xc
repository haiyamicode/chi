// Test error recovery within function blocks
func main() {
    var x = 42;
    
    // Broken statement missing semicolon and type
    var broken_var
    
    // Should recover and continue parsing
    var good_var = "hello";
    
    // Another broken statement
    if (x > 0
        // Missing closing paren and body
    
    // Should still parse this return statement
    return;
}