// Test malformed Shared<T> and reference operations
func main() {
    // Incomplete Shared construction
    var r1: Shared<int> = {

    // Shared with malformed type param
    var r2: Shared< = {};

    // ref_count call incomplete
    var count = r1.ref_count(

    // as_ref incomplete
    var val = r1.ref().

    // Nested Shared with errors
    var r3: Shared<Shared<int>> = {{

    // Reference with incomplete type
    var ref: &mut< = null;

    // Chained operations on Shared
    r1.set(.ref();

    // Invalid Shared method
    r1.invalid_method();

    // Incomplete optional with Shared
    var opt: ?Shared<int> =
}
