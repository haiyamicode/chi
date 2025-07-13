// Test basic language features in managed memory mode (.x files)
// This validates that core language constructs work correctly with escape analysis

struct TestStruct {
    id: int;
    name: string;
    
    mut func new(id: int, name: string) {
        this.id = id;
        this.name = name;
    }
    
    func display() {
        printf("TestStruct(id={}, name={})\n", this.id, this.name);
    }
    
    func delete() {
        printf("TestStruct({}) destroyed\n", this.id);
    }
}

enum Color {
    Red,
    Green, 
    Blue,
}

func test_basic_types() {
    println("=== Basic Types Test ===");
    
    var i: int = 42;
    var f: float = 3.14;
    var s: string = "hello managed mode";
    var b: bool = true;
    
    printf("int: {}\n", i);
    printf("float: {}\n", f); 
    printf("string: {}\n", s);
    printf("bool: {}\n", b);
}

func test_structs() {
    println("\n=== Structs Test ===");
    
    var obj: TestStruct = {1, "managed"};
    obj.display();
    
    var obj2: TestStruct = {2, "memory"};
    obj2.display();
}

func test_arrays() {
    println("\n=== Arrays Test ===");
    
    var arr: Array<int> = {};
    arr.add(10);
    arr.add(20);
    arr.add(30);
    
    printf("Array: [{}, {}, {}]\n", arr[0], arr[1], arr[2]);
    
    for var i = 0; i < 3; i++ {
        printf("arr[{}] = {}\n", i, arr[i]);
    }
}

func test_pointers() {
    println("\n=== Pointers Test ===");
    
    var value: int = 100;
    var ptr: *int = &value;
    
    printf("value = {}\n", value);
    printf("*ptr = {}\n", ptr!);
    
    ptr! = 200;
    printf("After modification: value = {}\n", value);
}

func test_enums() {
    println("\n=== Enums Test ===");
    
    var color: Color = Color.Red;
    printf("Color: {}\n", color.discriminator());
    
    color = Color.Blue;
    printf("Color: {}\n", color.discriminator());
}

func test_control_flow() {
    println("\n=== Control Flow Test ===");
    
    // If statements
    var x: int = 5;
    if x > 0 {
        println("x is positive");
    } else {
        println("x is not positive");
    }
    
    // For loops
    println("For loop:");
    for var i = 0; i < 3; i++ {
        printf("i = {}\n", i);
    }
    
    // While loops
    println("While loop:");
    var j: int = 0;
    while j < 3 {
        printf("j = {}\n", j);
        j++;
    }
}

func test_functions() {
    println("\n=== Functions Test ===");
    
    var obj: TestStruct = {99, "local"};
    printf("Creating local object: ");
    obj.display();
}

func test_lambdas() {
    println("\n=== Lambdas Test ===");
    
    var captured_value: int = 42;
    
    var simple_lambda = func() {
        println("Simple lambda executed");
    };
    
    var lambda_with_capture = func() int {
        return captured_value * 2;
    };
    
    simple_lambda();
    var result = lambda_with_capture();
    printf("Lambda with capture result: {}\n", result);
}

func main() {
    println("Testing basic language features in managed memory mode");
    
    test_basic_types();
    test_structs();
    test_arrays();
    test_pointers();
    test_enums();
    test_control_flow();
    test_functions();
    test_lambdas();
    
    println("\nAll basic features working in managed mode!");
}