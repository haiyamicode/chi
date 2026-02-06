// Test type parameter trait bounds for both functions and structs

interface Show {
    func show() string;
}

struct Point implements Show {
    x: int = 0;
    y: int = 0;
    
    func show() string {
        return string.format("({}, {})", this.x, this.y);
    }
}

struct Number implements Show {
    value: int = 0;
    
    func show() string {
        return string.format("Number({})", this.value);
    }
}

// Function with trait bound
func print_it<T: Show>(t: T) {
    printf("Result: {}\n", t.show());
}

// Function with reference trait bound
func print_ref<T: Show>(t: &T) {
    printf("Reference result: {}\n", t.show());
}

// Struct with single trait bound
struct Container<T: Show> {
    item: T = {};
    name: string = "";
    
    func show_container() string {
        return string.format("Container[{}]: {}", this.name, this.item.show());
    }
    
    func get_item() T {
        return this.item;
    }
}

// Struct with multiple type parameters and trait bounds
struct Pair<T: Show, U: Show> {
    first: T = {};
    second: U = {};
    
    func show_both() string {
        return string.format("Pair({}, {})", this.first.show(), this.second.show());
    }
}

func main() {
    printf("=== Type Parameter Trait Bounds Test ===\n");
    
    // Test function trait bounds
    printf("\n-- Function trait bounds --\n");
    var p: Point = {};
    print_it(p);
    
    // Test reference trait bounds
    printf("\n-- Reference trait bounds --\n");
    print_ref(&p);
    
    // Test struct trait bounds
    printf("\n-- Struct trait bounds --\n");
    var p2: Point = {.x = 10, .y = 20};
    var container: Container<Point> = {.item = p2, .name = "PointContainer"};
    printf("Single trait bound: {}\n", container.show_container());
    
    // Test accessing bound method through generic type
    var retrieved = container.get_item();
    printf("Retrieved item: {}\n", retrieved.show());
    
    // Test multiple type parameters with trait bounds
    var n: Number = {.value = 42};
    var pair: Pair<Point, Number> = {.first = p2, .second = n};
    printf("Multiple type params: {}\n", pair.show_both());
    
    // Test same interface, different implementations
    var num_container: Container<Number> = {.item = n, .name = "NumberContainer"};
    printf("Different type, same interface: {}\n", num_container.show_container());
    
    printf("\n=== All trait bound tests passed! ===\n");
}