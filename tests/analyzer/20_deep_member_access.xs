// Test deep member access that causes stack overflow in resolver
func main() {
    var long_var = 1;
    
    // This deep member access chain causes recursive resolver crash
    long_var.aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa.bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb.cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc;
}