// Test error handling: throw, try/catch, destructors during unwinding

import "std/ops" as ops;

struct MyError {
    msg: string = "";

    impl Error {
        func message() string {
            return this.msg;
        }
    }
}

struct OtherError {
    code: int = 0;

    impl Error {
        func message() string {
            return stringf("error code {}", this.code);
        }
    }
}

// --- Struct with destructor for testing cleanup during unwinding ---

struct Resource {
    name: string = "";

    mut func delete() {
        printf("Resource.delete({})\n", this.name);
    }

    impl ops.CopyFrom<Resource> {
        mut func copy_from(source: &Resource) {
            this.name = source.name;
        }
    }
}

// --- Basic throw/catch ---

func fail_with(msg: string) {
    throw new MyError{msg: msg};
}

func succeed() int {
    return 42;
}

func test_catch_all() {
    println("=== catch-all ===");

    // Throw is caught
    try fail_with("boom") catch {
        println("caught");
    };

    // No throw -- catch block not entered
    try succeed() catch {
        println("should not reach");
    };
    println("after success");
}

// --- Typed catch with variable binding ---

func test_typed_catch() {
    println("=== typed catch ===");

    // Catch specific error type with binding
    try fail_with("typed") catch (err: MyError) {
        printf("caught MyError: {}\n", err.message());
    };
}

// --- Void-returning functions ---

func void_fail() {
    throw new MyError{msg: "void fail"};
}

func void_succeed() {
    println("void ok");
}

func test_void_try() {
    println("=== void try ===");

    try void_fail() catch {
        println("caught void");
    };

    try void_succeed() catch {
        println("should not reach");
    };
}

// --- Destructor cleanup during stack unwinding ---
// When a function with local destructors calls a throwing function,
// the local destructors run during unwinding before the catch.

func throws_immediately() {
    throw new MyError{msg: "unwind"};
}

func outer_with_resources() {
    var r1 = Resource{name: "A"};
    var r2 = Resource{name: "B"};
    throws_immediately();
}

func middle_layer() {
    var r = Resource{name: "mid"};
    outer_with_resources();
}

func test_destructor_unwind() {
    println("=== destructor unwind ===");
    try middle_layer() catch {
        println("caught after unwind");
    };
}

// --- Error struct with string field (tests cleanup of caught error) ---

func test_error_cleanup() {
    println("=== error cleanup ===");
    try fail_with("heap string error") catch (err: MyError) {
        printf("msg: {}\n", err.message());
    };
    println("after cleanup");
}

// --- Multiple try/catch in sequence ---

func test_sequential() {
    println("=== sequential ===");

    try fail_with("first") catch {
        println("caught first");
    };

    try fail_with("second") catch {
        println("caught second");
    };

    try succeed() catch {
        println("should not reach");
    };
    println("all done");
}

// --- Nested try/catch ---

func inner_tries() {
    try fail_with("inner") catch {
        println("inner caught");
    };
    println("between");
    try fail_with("inner2") catch {
        println("inner2 caught");
    };
}

func test_nested() {
    println("=== nested ===");
    try inner_tries() catch {
        println("outer should not reach");
    };
    println("nested done");
}

// --- Try with catch on function that doesn't throw ---

func no_throw() string {
    return "safe";
}

func test_no_throw() {
    println("=== no throw ===");
    try no_throw() catch {
        println("should not reach");
    };
    println("no throw ok");
}

// --- Result mode: try without catch block ---

func test_result_mode() {
    println("=== result mode ===");

    // try f() → Result<int, &Error> — success case
    var result = try succeed();
    if result.error {
        println("unexpected error");
    } else {
        printf("value = {}\n", result.value);
    }

    // try f() → Result<Unit, &Error> — error case (void fn → Result<Unit, &Error>)
    var result2 = try fail_with("oops");
    if result2.error {
        printf("got error: {}\n", result2.error.message());
    } else {
        println("should not reach");
    }
}

// --- Typed catch without block → Result<T, &MyError> ---

func test_typed_result() {
    println("=== typed result ===");

    // Error case
    var result = try fail_with("typed result") catch MyError;
    if result.error {
        printf("caught: {}\n", result.error.message());
    } else {
        println("should not reach");
    }

    // Success case
    var result2 = try succeed() catch MyError;
    if result2.error {
        println("should not reach");
    } else {
        printf("value = {}\n", result2.value);
    }
}

// --- Catch block: fallback values, return, re-throw ---

func fail_int(msg: string) int {
    throw new MyError{msg: msg};
}

func test_catch_block() {
    println("=== catch block ===");

    // Catch-all with fallback expression
    var x = try fail_int("risky") catch {
        -1
    };
    printf("fallback = {}\n", x);

    // Success path — catch block not entered
    var y = try succeed() catch {
        -1
    };
    printf("success = {}\n", y);

    // Typed catch with fallback expression using the error
    var z = try fail_int("bad") catch (err: MyError) {
        printf("handling: {}\n", err.message());
        0
    };
    printf("typed fallback = {}\n", z);
}

// Re-throw: catch one error, throw a different one
func test_rethrow() {
    println("=== rethrow ===");
    try fail_with("original") catch (err: MyError) {
        printf("caught: {}\n", err.message());
        throw new OtherError{code: 42};
    };
}

// Branching in catch: one path returns fallback, other re-throws
func branching_catch(rethrow: bool) int {
    var x = try fail_int("boom") catch (err: MyError) {
        if rethrow {
            throw new OtherError{code: 123};
        }
        -1
    };
    return x;
}

// Return from catch block exits the enclosing function
func test_catch_return() int {
    try fail_with("bail") catch {
        return -1;
    };
    return 99;
}

func main() {
    test_catch_all();
    test_typed_catch();
    test_void_try();
    test_destructor_unwind();
    test_error_cleanup();
    test_sequential();
    test_nested();
    test_no_throw();
    test_result_mode();
    test_typed_result();
    test_catch_block();

    // Re-throw propagates out — catch it here
    var rethrow_result = try test_rethrow() catch OtherError;
    if rethrow_result.error {
        printf("re-caught: {}\n", rethrow_result.error.message());
    }

    // Branching catch: fallback vs re-throw
    printf("branch fallback = {}\n", branching_catch(false));
    var branch_result = try branching_catch(true) catch OtherError;
    if branch_result.error {
        printf("branch rethrow: {}\n", branch_result.error.message());
    }

    // Return from catch exits the function
    printf("early return = {}\n", test_catch_return());
}

