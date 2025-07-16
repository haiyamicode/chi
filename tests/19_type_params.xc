// Test function and method type parameters
// This test verifies:
// 1. Top-level generic functions with type parameters
// 2. Generic methods with their own type parameters (separate from struct type params)
// 3. Method type parameter resolution and specialization
// 4. Chaining of generic method calls with different type parameters

// Top-level generic function with type parameter
func identity<T>(value: T) T {
    return value;
}

// Generic function that transforms values
func transform<T, U>(value: T, transformer: func(value: T) U) U {
    return transformer(value);
}

struct Container<T> {
    value: T;

    func new(value: T) {
        this.value = value;
    }
    
    func get() T {
        return this.value;
    }
    
    func zmap<U>(transform: func(value: T) U) Container<U> {
        return {transform(this.value)};
    }
}

func main() {
    // Test top-level generic function
    var int_result = identity<int>(42);
    printf("identity<int>(42) = {}\n", int_result);
    
    var char_result = identity<char>('A');
    printf("identity<char>('A') = {}\n", char_result);
    
    // // Test generic transform function
    var doubled = transform<int, int>(5, func(x: int) int { return x * 2; });
    printf("transform<int, int>(5, double) = {}\n", doubled);
    
    // Test generic struct with method type parameters
    var container: Container<int> = {65};
    var zmap_result = container.zmap<char>(func (value: int) char {
        return (value + 10) as char;
    });
    var final_value = zmap_result.get();
    printf("final value: {}\n", final_value);
    
    // Test another method type parameter usage
    var float_container = container.zmap<float>(func(i: int) float { 
        return i as float * 0.5; 
    });
    printf("float result: {}\n", float_container.get());
}