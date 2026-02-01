struct GCBox {
    id: int;
    
    mut func new(id: int) {
        this.id = id;
        printf("GCBox({}) created\n", this.id);
    }
    
    func delete() {
        printf("GCBox({}) destroyed\n", this.id);
    }
}

func get_escaped_ptr() *GCBox {
    // This object MUST escape because we return a pointer to it
    var obj: GCBox = {100};
    return &obj;
}

func get_non_escaped_value() int {
    // This object should NOT escape - it's only used locally
    var obj: GCBox = {200};
    return obj.id;
}

func test_lambda_capture() {
    // This object MUST escape because lambda captures it and lambda outlives the scope
    var captured: GCBox = {300};
    var lambda = func() int {
        return captured.id;
    };
    println("Lambda created with captured object");
    var result = lambda();
    printf("Lambda executed successfully, captured object id: {}\n", result);
}

// This test validates Chi's escape analysis in managed memory mode (.x files).
// Key behaviors verified:
// 1. Non-escaping objects: Stack allocated, immediately destroyed
// 2. Escaping objects: Heap allocated, GC managed  
// 3. Pointer validity: Escaped pointers remain valid after scope ends
// 4. Lambda captures: Properly handle object escape via closures
// 5. Function returns: Objects escaping via return values
func main() {
    println("=== Testing Stack vs Heap Allocation ===");
    
    // Test 1: Non-escaping object (should be stack-allocated, destroyed immediately)
    println("Test 1: Non-escaping object");
    {
        var local: GCBox = {1};
        println("Local object created, should be destroyed at scope end");
    }
    println("Scope ended - non-escaping object should be destroyed by now");
    
    // Test 2: Escaping via assignment (should be heap-allocated)
    println("\nTest 2: Object escaping via pointer assignment");
    var escaped_ptr: *GCBox = null;
    {
        var local: GCBox = {2};
        escaped_ptr = &local;
        printf("Inside scope - local.id: {}, escaped_ptr->id: {}\n", local.id, escaped_ptr!.id);
        println("Local object escapes via pointer assignment");
    }
    println("Scope ended - escaped object should still be alive");
    printf("CRITICAL TEST: Accessing escaped pointer after scope: GCBox({}).id = {}\n", escaped_ptr!.id, escaped_ptr!.id);
    
    // Test 3: Escaping via return value
    println("\nTest 3: Object escaping via return value");
    var returned_ptr = get_escaped_ptr();
    printf("CRITICAL TEST: Function returned pointer to GCBox({}), accessing id: {}\n", returned_ptr!.id, returned_ptr!.id);
    
    // Test 4: Non-escaping function call
    println("\nTest 4: Non-escaping function call");
    var value = get_non_escaped_value();
    printf("Non-escaping function returned value: {} (object was destroyed)\n", value);
    
    // Test 5: Lambda capture escape
    println("\nTest 5: Lambda capture escape");
    test_lambda_capture();
    
    // Test 6: Use-after-scope validation
    println("\nTest 6: Use-after-scope validation");
    var ptr1: *GCBox = null;
    var ptr2: *GCBox = null;
    {
        var a: GCBox = {6};
        var b: GCBox = {7};
        ptr1 = &a;
        ptr2 = &b;
        printf("Inside scope - accessing a.id: {}, b.id: {}\n", a.id, b.id);
        printf("Inside scope - accessing via pointers: ptr1->id={}, ptr2->id={}\n", ptr1!.id, ptr2!.id);
        println("Both objects created and pointers assigned");
    }
    println("Both objects should still be accessible:");
    printf("CRITICAL TEST: After scope ended - ptr1->id={}, ptr2->id={}\n", ptr1!.id, ptr2!.id);
    printf("CRITICAL TEST: Accessing values - GCBox({}) and GCBox({}) both accessible!\n", ptr1!.id, ptr2!.id);
    
    // Clean up explicitly
    println("\n=== Cleanup Phase ===");
    escaped_ptr = null;
    returned_ptr = null;
    ptr1 = null;
    ptr2 = null;

    // Final gc objects
    println("Final gc objects...");
    for var i = 0; i < 5; i++ {
        var temp: GCBox = {1000 + i};
    }

    println("Test completed");
}