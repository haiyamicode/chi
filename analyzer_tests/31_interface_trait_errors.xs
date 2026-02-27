// Test malformed interfaces and trait bounds
interface Show {
    func show() string

interface Broken {
    // Missing function
    func

struct Point implements Show {
    x: int;

    // Missing interface method implementation
    func other() {
}

// Generic with malformed trait bound
func print_it<T: > (t: T) {
}

// Multiple trait bounds with errors
func complex<T: Show + > (t: T) {
}

// Trait bound with invalid type
struct Container<T: NonExistent> {
    item: T;
}

func main() {
    // Call generic function with wrong constraints
    var p: Point = {1
    print_it<int>(

    // Incomplete trait bound check
    var c: Container<
}
