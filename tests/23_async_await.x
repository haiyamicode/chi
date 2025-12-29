// Test async/await functionality

// Simple async function that returns a value (no await - synchronous resolution)
async func getValue() Promise<int> {
    return 42;
}

// Async function that doubles a value (no await - synchronous resolution)
async func doubleValue(x: int) Promise<int> {
    return x * 2;
}

// Async function that uses await (synchronous - getValue resolves immediately)
async func processValue() Promise<int> {
    var value = await getValue();
    var doubled = await doubleValue(value);
    return doubled;
}

// Chained async calls (synchronous - all resolve immediately)
async func chainedAsync() Promise<int> {
    var a = await getValue();
    var b = await doubleValue(a);
    var c = await doubleValue(b);
    return c;
}

func main() {
    // Test basic async function call (sync)
    var p1 = getValue();
    printf("getValue() returned a Promise\n");
    printf("p1.is_resolved={}, p1.get_value={}\n", p1.is_resolved(), p1.get_value());

    // Test async with parameter (sync)
    var p2 = doubleValue(21);
    printf("doubleValue(21) returned a Promise\n");
    printf("p2.is_resolved={}, p2.get_value={}\n", p2.is_resolved(), p2.get_value());

    // Test nested await (sync)
    var p3 = processValue();
    printf("processValue() returned a Promise, is_resolved={}, get_value={}\n", p3.is_resolved(), p3.get_value());

    // Test chained awaits (sync)
    var p4 = chainedAsync();
    printf("chainedAsync() returned a Promise, is_resolved={}, get_value={}\n", p4.is_resolved(), p4.get_value());

    printf("async/await test complete!\n");
}
