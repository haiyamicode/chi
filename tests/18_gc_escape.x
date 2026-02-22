struct GCBox {
    id: int;

    mut func new(id: int) {
        this.id = id;
        printf("[gc] GCBox({}) created\n", this.id);
    }

    func delete() {
        printf("[gc] GCBox({}) destroyed\n", this.id);
    }
}

struct Holder {
    ref: &GCBox;

    func new(r: &GCBox) {
        this.ref = r;
    }
}

func get_escaped_ref() &GCBox {
    var obj = GCBox{100};
    return &obj;
}

func get_non_escaped_value() int {
    var obj = GCBox{200};
    return obj.id;
}

func test_lambda_capture() {
    var captured = GCBox{300};
    var lambda = func () int {
        return captured.id;
    };
    println("Lambda created with captured object");
    var result = lambda();
    printf("Lambda executed successfully, captured object id: {}\n", result);
}

// Transitive: var p = &obj; return p
func get_via_reassign() &GCBox {
    var obj = GCBox{400};
    var p = &obj;
    return p;
}

// Transitive: struct field holds ref
func get_via_struct() Holder {
    var obj = GCBox{500};
    var h = Holder{&obj};
    return h;
}

// Transitive: deep chain var a = &x; var b = a; var c = b; return c
func get_via_chain() &GCBox {
    var obj = GCBox{600};
    var a = &obj;
    var b = a;
    var c = b;
    return c;
}

func main() {
    println("=== Testing Stack vs Heap Allocation ===");

    println("Test 1: Non-escaping object");
    {
        var local = GCBox{1};
        println("Local object created, should be destroyed at scope end");
    }
    println("Scope ended - non-escaping object should be destroyed by now");

    println("\nTest 2: Object escaping via ref assignment");
    var escaped: &GCBox = null;
    {
        var local = GCBox{2};
        escaped = &local;
        printf("Inside scope - local.id: {}, escaped.id: {}\n", local.id, escaped.id);
        println("Local object escapes via ref assignment");
    }
    println("Scope ended - escaped object should still be alive");
    printf("CRITICAL TEST: Accessing escaped ref after scope: GCBox({}).id = {}\n", escaped.id, escaped.id);

    println("\nTest 3: Object escaping via return value");
    var returned = get_escaped_ref();
    printf("CRITICAL TEST: Function returned ref to GCBox({}), accessing id: {}\n", returned.id, returned.id);

    println("\nTest 4: Non-escaping function call");
    var value = get_non_escaped_value();
    printf("Non-escaping function returned value: {} (object was destroyed)\n", value);

    println("\nTest 5: Lambda capture escape");
    test_lambda_capture();

    println("\nTest 6: Use-after-scope validation");
    var ref1: &GCBox = null;
    var ref2: &GCBox = null;
    {
        var a = GCBox{6};
        var b = GCBox{7};
        ref1 = &a;
        ref2 = &b;
        printf("Inside scope - accessing a.id: {}, b.id: {}\n", a.id, b.id);
        printf("Inside scope - accessing via refs: ref1.id={}, ref2.id={}\n", ref1.id, ref2.id);
        println("Both objects created and refs assigned");
    }
    println("Both objects should still be accessible:");
    printf("CRITICAL TEST: After scope ended - ref1.id={}, ref2.id={}\n", ref1.id, ref2.id);
    printf("CRITICAL TEST: Accessing values - GCBox({}) and GCBox({}) both accessible!\n", ref1.id, ref2.id);

    // Release earlier refs before transitive tests
    escaped = null;
    println("escaped released");
    returned = null;
    println("returned released");
    ref1 = null;
    ref2 = null;
    println("refs released");

    println("\nTest 7: Transitive escape via reassignment");
    var reassigned = get_via_reassign();
    printf("CRITICAL TEST: reassigned.id = {}\n", reassigned.id);

    println("\nTest 8: Transitive escape via struct field");
    var holder = get_via_struct();
    printf("CRITICAL TEST: holder.ref.id = {}\n", holder.ref.id);

    println("\nTest 9: Transitive escape via deep chain");
    var chained = get_via_chain();
    printf("CRITICAL TEST: chained.id = {}\n", chained.id);

    println("\n=== Cleanup Phase ===");

    println("Final gc objects...");
    for var i = 0; i < 5; i++ {
        var temp = GCBox{1000 + i};
    }

    println("Test completed");
}

