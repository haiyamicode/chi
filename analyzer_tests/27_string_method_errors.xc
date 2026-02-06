// Test malformed string operations and method calls
func main() {
    // Incomplete string.format call
    var s1 = string.format(

    // String method on invalid receiver
    var s2 = 123.is_empty();

    // Missing closing quote
    var s3 = "hello

    // Chained method calls with errors
    var s4 = "test".add(.is_empty();

    // format with malformed arguments
    var s5 = string.format("test", ,, );

    // Invalid escape sequence leading to incomplete string
    var s6 = "test\

    // Method call on string literal incomplete
    var s7 = "hello".

    // CString errors
    var cstr = s1.to_cstring(
}
