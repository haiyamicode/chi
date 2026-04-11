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

struct DetailedError {
    line: ?int = null;

    impl Error {
        func message() string {
            return "detail";
        }
    }
}

// --- Struct with destructor for testing cleanup during unwinding ---

struct Resource {
    name: string = "";

    mut func delete() {
        printf("Resource.delete({})\n", this.name);
    }

    impl ops.Copy {
        mut func copy(source: &This) {
            this.name = source.name;
        }
    }
}

// --- Basic throw/catch ---

func fail_with(msg: string) {
    throw new MyError{:msg};
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
    try fail_with("typed") catch MyError as err {
        printf("caught MyError: {}\n", err.message());
    };
}

func fail_detailed() {
    throw new DetailedError{line: 7};
}

func test_catch_binding_narrow() {
    println("=== catch binding narrow ===");

    try fail_detailed() catch DetailedError as err {
        if let line = err.line {
            printf("line = {}\n", line);
        }
        printf("detail = {}\n", err.message());
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
    try fail_with("heap string error") catch MyError as err {
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

func test_panic_tryable() {
    println("=== panic tryable ===");

    try panic("boom") catch PanicError as err {
        printf("panic caught: {}\n", err.message());
    };

    try assert(false, "bad assert") catch PanicError as err {
        printf("assert caught: {}\n", err.message());
    };
}

// --- Result mode: try without catch block ---

func test_result_mode() {
    println("=== result mode ===");

    // try f() → Result<int, Shared<Error>> — success case
    var result = try succeed();
    switch result {
        Ok(value) => printf("value = {}\n", value),
        Err => println("unexpected error")
    }

    // try f() → Result<(), Shared<Error>> — error case (void fn → Result<(), Shared<Error>>)
    var result2 = try fail_with("oops");
    switch result2 {
        Err(err) => printf("got error: {}\n", err.message()),
        Ok => println("should not reach")
    }
}

// --- Typed catch without block → Result<T, Shared<Error>> with type filtering ---

func test_typed_result() {
    println("=== typed result ===");

    // Error case
    var result = try fail_with("typed result") catch MyError;
    switch result {
        Err(err) => printf("caught: {}\n", err.message()),
        Ok => println("should not reach")
    }

    // Success case
    var result2 = try succeed() catch MyError;
    switch result2 {
        Ok(value) => printf("value = {}\n", value),
        Err => println("should not reach")
    }
}

// --- Catch block: fallback values, return, re-throw ---

func fail_int(msg: string) int {
    throw new MyError{:msg};
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
    var z = try fail_int("bad") catch MyError as err {
        printf("handling: {}\n", err.message());
        0
    };
    printf("typed fallback = {}\n", z);
}

// --- Try blocks: try { ... } catch { ... } ---

func test_try_block_catch_all() {
    println("=== try block catch-all ===");

    // Block throws, catch-all handles it
    try {
        fail_with("block throw");
    } catch {
        println("block caught");
    };

    // Block succeeds, catch not entered
    try {
        var v = succeed();
        printf("block ok = {}\n", v);
    } catch {
        println("should not reach");
    };
}

func test_try_block_typed() {
    println("=== try block typed ===");

    // Typed catch inside try-block
    try {
        fail_with("typed block");
    } catch MyError as err {
        printf("block typed: {}\n", err.message());
    };

    // Typed catch mismatch falls through
    var result = try {
        fail_with("mismatch");
    } catch OtherError;
    switch result {
        Err(err) => printf("mismatch propagated: {}\n", err.message()),
        Ok => println("should not reach")
    }
}

func test_try_block_return() int {
    try {
        fail_with("bail");
    } catch {
        return -1;
    };
    return 99;
}

func test_try_block_destructor() {
    println("=== try block destructor ===");
    try {
        var r = Resource{name: "inner"};
        fail_with("unwind block");
    } catch {
        println("after block unwind");
    };
}

// Result mode: try { ... } catch [Type] → Result<T, Shared<Error>>

func test_try_block_result_mode() {
    println("=== try block result mode ===");

    // Success path → Ok
    var ok_result = try {
        succeed()
    } catch MyError;
    switch ok_result {
        Ok(v) => printf("ok = {}\n", v),
        Err => println("should not reach")
    }

    // Error matches filter → Err
    var err_result = try {
        fail_with("result block");
    } catch MyError;
    switch err_result {
        Err(err) => printf("err = {}\n", err.message()),
        Ok => println("should not reach")
    }

    // Error doesn't match filter → still Err (wrapped Shared<Error>)
    var mismatch_result = try {
        fail_with("mismatch");
    } catch OtherError;
    switch mismatch_result {
        Err(err) => printf("mismatch = {}\n", err.message()),
        Ok => println("should not reach")
    }
}

// Bare catch-all: try { ... } without catch → Result<T, Shared<Error>>
func test_try_block_bare_catchall() {
    println("=== try block bare catch-all ===");

    var ok_result = try {
        succeed()
    };
    switch ok_result {
        Ok(v) => printf("ok = {}\n", v),
        Err => println("should not reach")
    }

    var err_result = try {
        fail_with("bare catch-all");
        0
    };
    switch err_result {
        Err(err) => printf("err = {}\n", err.message()),
        Ok => println("should not reach")
    }

    var void_result = try {
        fail_with("void bare");
    };
    switch void_result {
        Err(err) => printf("void err = {}\n", err.message()),
        Ok => println("void ok")
    }

    var void_ok = try {
        println("no throw");
    };
    switch void_ok {
        Ok => println("void ok success"),
        Err => println("should not reach")
    }
}

// Multiple sequential try blocks in one function
func test_try_block_sequential() {
    println("=== try block sequential ===");

    try {
        fail_with("first");
    } catch {
        println("caught first");
    };

    try {
        fail_with("second");
    } catch {
        println("caught second");
    };

    // Success in middle
    try {
        var v = succeed();
        printf("success = {}\n", v);
    } catch {
        println("should not reach");
    };

    try {
        fail_with("third");
    } catch {
        println("caught third");
    };
    println("all done");
}

// Re-throw: catch one error, throw a different one
func test_rethrow() {
    println("=== rethrow ===");
    try fail_with("original") catch MyError as err {
        printf("caught: {}\n", err.message());
        throw new OtherError{code: 42};
    };
}

// Branching in catch: one path returns fallback, other re-throws
func branching_catch(rethrow: bool) int {
    var x = try fail_int("boom") catch MyError as err {
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

func deep_thrower() {
    throw new MyError{msg: "deep"};
}

func mid_caller() {
    deep_thrower();
}

func test_error_trace() {
    println("=== error trace ===");

    // Typed catch
    try {
        mid_caller();
    } catch MyError as err {
        let t = err.trace();
        printf("has trace: {}\n", t.length > 0);
        printf("has deep_thrower: {}\n", t.contains("deep_thrower"));
        printf("has mid_caller: {}\n", t.contains("mid_caller"));
    };

    // Result mode
    var result = try {
        mid_caller();
        0
    } catch MyError;
    switch result {
        Err(err) => {
            let t = err.trace();
            printf("result has trace: {}\n", t.length > 0);
            printf("result has deep_thrower: {}\n", t.contains("deep_thrower"));
        },
        Ok => {}
    }
}

func main() {
    test_catch_all();
    test_typed_catch();
    test_catch_binding_narrow();
    test_void_try();
    test_destructor_unwind();
    test_error_cleanup();
    test_sequential();
    test_nested();
    test_no_throw();
    test_panic_tryable();
    test_result_mode();
    test_typed_result();
    test_catch_block();
    test_try_block_catch_all();
    test_try_block_typed();
    printf("block return = {}\n", test_try_block_return());
    test_try_block_destructor();
    test_try_block_result_mode();
    test_try_block_bare_catchall();
    test_try_block_sequential();

    // Re-throw propagates out — catch it here
    var rethrow_result = try test_rethrow() catch OtherError;
    switch rethrow_result {
        Err(err) => printf("re-caught: {}\n", err.message()),
        Ok => {}
    }

    // Branching catch: fallback vs re-throw
    printf("branch fallback = {}\n", branching_catch(false));
    var branch_result = try branching_catch(true) catch OtherError;
    switch branch_result {
        Err(err) => printf("branch rethrow: {}\n", err.message()),
        Ok => {}
    }

    // Return from catch exits the function
    printf("early return = {}\n", test_catch_return());

    test_error_trace();
}
