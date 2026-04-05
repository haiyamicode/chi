// Complex malformed expressions that previously caused segfaults
func main() {
    var x = obj...field;  // Multiple dots and undeclared identifier
    var y = arr[[[;       // Nested brackets and undeclared identifier
}