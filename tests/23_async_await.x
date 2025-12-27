// Test async/await functionality

// Simple async function that returns a value
async func getValue() Promise<int> {
    return 42;
}

// Async function that doubles a value
async func doubleValue(x: int) Promise<int> {
    return x * 2;
}

// Async function that uses await
async func processValue() Promise<int> {
    var value = await getValue();
    var doubled = await doubleValue(value);
    return doubled;
}

// Chained async calls
async func chainedAsync() Promise<int> {
    var a = await getValue();
    var b = await doubleValue(a);
    var c = await doubleValue(b);
    return c;
}

func main() {
    // Test basic async function call
    var p1 = getValue();
    printf("getValue() returned a Promise\n");

    // Test async with parameter
    var p2 = doubleValue(21);
    printf("doubleValue(21) returned a Promise\n");

    // Test nested await
    var p3 = processValue();
    printf("processValue() returned a Promise\n");

    // Test chained awaits
    var p4 = chainedAsync();
    printf("chainedAsync() returned a Promise\n");

    printf("async/await test complete!\n");
}
