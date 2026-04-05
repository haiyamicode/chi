// Test recovery from multiple types of errors in same file
import "std/io"

// Broken struct
struct Broken {
    field1: int
    // Missing semicolon, comma, or proper field separator
    field2 string
    // Missing type colon
    field3
}

// Incomplete enum
enum Status
    // Missing opening brace

// Should still parse this correctly
func good_function() {
    var msg = "This should work";
}

// Broken function with recovery
func broken_func() {
    var x = 1 + ;
    // Incomplete expression
    
    // Should recover here
    var y = 2;
}