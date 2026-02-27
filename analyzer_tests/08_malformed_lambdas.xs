// Malformed lambdas
func main() {
    var f = func (x: int {
        return x + ;
    
    var g = func () -> {
        if (true {
            var nested = func (