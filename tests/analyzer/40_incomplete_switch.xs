// Incomplete switch expressions (was causing infinite loops in parse_switch_expr)
func main() {
    switch
    switch;
    var x = 1;
    switch x {
    switch x { case
    switch x { case 1
    switch x { case 1:
}
