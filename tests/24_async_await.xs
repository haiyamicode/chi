import "std/ops" as ops;
import "std/time" as time;

// --- Promise API ---

func test_manual_promise() {
    println("=== Manual Promise ===");
    var p = Promise<int>{};
    p.then(func (val: int) {
        printf("Callback: {}\n", val);
    });
    println("Resolving...");
    p.resolve(100);

    // then on already-resolved promise
    var p2 = Promise<int>{};
    p2.resolve(200);
    p2.then(func (val: int) {
        printf("Already resolved: {}\n", val);
    });
}

func test_promise_state() {
    println("=== Promise state ===");
    var p = Promise<int>{};
    printf("before: {}\n", p.is_resolved());
    p.resolve(42);
    printf("after: {}\n", p.is_resolved());
    printf("value: {}\n", p.value()!);
}

func test_then_chain() {
    println("=== Then chain ===");
    // Multi-step chain with type changes: int -> string -> int -> void
    var p = Promise<int>{};
    var p2 = p.then(func (v: int) string {
        return stringf("n={}", v);
    });
    var p3 = p2.then(func (s: string) int {
        return s.length;
    });
    var p4 = p3.then(func (v: int) {
        printf("chain result: {}\n", v);
    });
    printf(
        "before resolve: {} {} {} {}\n",
        p.is_resolved(),
        p2.is_resolved(),
        p3.is_resolved(),
        p4.is_resolved()
    );
    p.resolve(42);
    printf(
        "after resolve: {} {} {} {}\n",
        p.is_resolved(),
        p2.is_resolved(),
        p3.is_resolved(),
        p4.is_resolved()
    );
    printf("p2: '{}'\n", p2.value()!);
    printf("p3: {}\n", p3.value()!);

    // Chain on already-resolved promise
    var r = Promise<int>{};
    r.resolve(7);
    var r2 = r.then(func (v: int) int {
        return v * 3;
    });
    var r3 = r2.then(func (v: int) string {
        return stringf("got {}", v);
    });
    printf("already resolved chain: '{}'\n", r3.value()!);
}

func test_promise_make() {
    println("=== Promise.make ===");
    var p = Promise<int>.make(func (resolve) {
        resolve(123);
    });
    printf("value: {}\n", p.value()!);
}

func test_promise_string() {
    println("=== Promise<string> ===");
    var p1 = Promise<string>.make(func (resolve) {
        resolve("hello world");
    });
    printf("p1: '{}'\n", p1.value()!);

    var prefix = "hello";
    var p2 = Promise<string>.make(
        func [prefix] (resolve) {
            resolve(prefix + " from capture");
        }
    );
    printf("p2: '{}'\n", p2.value()!);

    var p3 = Promise<string>{};
    p3.then(func (v: string) {
        printf("deferred: '{}'\n", v);
    });
    p3.resolve("resolved later");
}

struct Container {
    static func make_promise(msg: string) Promise<string> {
        return Promise<string>.make(func [msg] (resolve) {
            resolve(msg);
        });
    }
}

func test_promise_in_struct() {
    println("=== Promise in struct ===");
    var p = Container.make_promise("from struct");
    printf("value: '{}'\n", p.value()!);
}

// --- Async/await helpers ---

async func double_it(x: int) Promise<int> {
    return x * 2;
}

async func add_async(a: int, b: int) Promise<int> {
    return a + b;
}

async func greet(name: string) Promise<string> {
    return "hello " + name;
}

async func delayed_number(value: int) Promise<int> {
    await time.sleep(1);
    return value;
}

// --- Async no await ---

async func simple_async(x: int) Promise<int> {
    return x + 10;
}

func test_async_no_await() {
    println("=== Async no await ===");
    var p = simple_async(7);
    printf("value: {}\n", p.value()!);
}

// --- Await: var decl ---

async func do_var_decl() Promise<int> {
    var x = await double_it(5);
    return x;
}

func test_var_decl() {
    println("=== Var decl ===");
    printf("value: {}\n", do_var_decl().value()!);
}

// --- Await: bare ---

async func do_bare_await() Promise {
    await time.sleep(1);
    println("bare ok");
}

func test_bare_await() {
    println("=== Bare await ===");
    do_bare_await();
}

// --- Await: func arg ---

async func do_await_in_func_arg() Promise<int> {
    var result = await double_it(7);
    printf("in func arg: {}\n", result);
    return result;
}

func test_await_in_func_arg() {
    println("=== Await in func arg ===");
    printf("value: {}\n", do_await_in_func_arg().value()!);
}

// --- Await: binop ---

async func do_await_in_binop() Promise<int> {
    var x = await double_it(3) + 100;
    return x;
}

func test_await_in_binop() {
    println("=== Await in binop ===");
    printf("value: {}\n", do_await_in_binop().value()!);
}

// --- Await: multiple sequential ---

async func do_multiple_awaits() Promise<int> {
    var a = await double_it(1);
    var b = await double_it(2);
    var c = await double_it(3);
    return a + b + c;
}

func test_multiple_awaits() {
    println("=== Multiple awaits ===");
    printf("value: {}\n", do_multiple_awaits().value()!);
}

// --- Await: locals across boundaries ---

async func do_locals_across() Promise<int> {
    var before = 42;
    var x = await double_it(10);
    return before + x;
}

func test_locals_across() {
    println("=== Locals across await ===");
    printf("value: {}\n", do_locals_across().value()!);
}

// --- Await: chain ---

async func compute_chain(start: int) Promise<int> {
    var step1 = await double_it(start);
    var step2 = await double_it(step1);
    var step3 = await double_it(step2);
    return step3;
}

func test_chain() {
    println("=== Chain ===");
    printf("value: {}\n", compute_chain(5).value()!);
}

// --- Await: nested async ---

async func inner_chain(x: int) Promise<int> {
    var a = await double_it(x);
    return a + 1;
}

async func do_nested_chain() Promise<int> {
    var result = await inner_chain(10);
    return result;
}

func test_nested_chain() {
    println("=== Nested chain ===");
    printf("value: {}\n", do_nested_chain().value()!);
}

// --- Await: return await ---

async func do_return_await() Promise<int> {
    return await double_it(25);
}

func test_return_await() {
    println("=== Return await ===");
    printf("value: {}\n", do_return_await().value()!);
}

// --- Await: comparison ---

async func do_await_comparison() Promise<bool> {
    var big = await double_it(5) > 8;
    return big;
}

func test_await_comparison() {
    println("=== Await in comparison ===");
    printf("value: {}\n", do_await_comparison().value()!);
}

// --- Await: interleaved with non-await stmts ---

async func do_interleaved() Promise<int> {
    var a = await double_it(1);
    var b = await double_it(a);
    var c = await double_it(b);
    return a + b + c;
}

func test_interleaved() {
    println("=== Interleaved ===");
    printf("value: {}\n", do_interleaved().value()!);
}

// --- Await: string ---

async func do_await_string() Promise<string> {
    var msg = await greet("world");
    return msg;
}

func test_await_string() {
    println("=== Await string ===");
    printf("value: '{}'\n", do_await_string().value()!);
}

// --- Await: string concat ---

async func do_string_concat() Promise<string> {
    var msg = await greet("world") + "!";
    return msg;
}

func test_string_concat() {
    println("=== String concat ===");
    printf("value: '{}'\n", do_string_concat().value()!);
}

// --- Await: as last stmt (no code after) ---

async func do_await_last() Promise<int> {
    var x = await double_it(99);
    return x;
}

func test_await_last() {
    println("=== Await as last ===");
    printf("value: {}\n", do_await_last().value()!);
}

// --- Await: double nested chain ---

async func double_chain(x: int) Promise<int> {
    var step1 = await double_it(x);
    var step2 = await double_it(step1);
    return step2;
}

async func do_double_chain() Promise<int> {
    return await double_chain(3);
}

func test_double_chain() {
    println("=== Double chain ===");
    printf("value: {}\n", do_double_chain().value()!);
}

// --- Await: nested if/return control flow ---

async func do_branchy_returns(flag1: bool, flag2: bool, base: int) Promise<int> {
    var a = await delayed_number(1);
    var seed = base + a;

    if flag1 {
        if flag2 {
            return seed + 1000;
        }

        var b = await delayed_number(10);
        return seed + b + 100;
    }

    var c = await delayed_number(20);
    if flag2 {
        return seed + c + 10;
    }

    return seed + c + 20;
}

async func delayed_flag(flag: bool, ms: int) Promise<bool> {
    var y = await time.sleep(ms);
    return flag;
}

async func do_condition_await_returns(flag1: bool, flag2: bool, base: int) Promise<int> {
    var a = await delayed_number(base);

    if await delayed_flag(flag1, 205) {
        if await delayed_flag(flag2, 206) {
            return a + 1000;
        }

        var b = await delayed_number(10);
        return a + b + 100;
    }

    var c = await delayed_number(20);
    if await delayed_flag(flag2, 207) {
        return a + c + 10;
    }

    return a + c + 20;
}

async func run_condition_await_returns_cases() Promise {
    printf("cond tt={}\n", await do_condition_await_returns(true, true, 5));
    printf("cond tf={}\n", await do_condition_await_returns(true, false, 5));
    printf("cond ft={}\n", await do_condition_await_returns(false, true, 5));
    printf("cond ff={}\n", await do_condition_await_returns(false, false, 5));
}

async func do_else_if_await_returns(flag1: bool, flag2: bool, base: int) Promise<int> {
    if await delayed_flag(flag1, 226) {
        return base + 10;
    } else if await delayed_flag(flag2, 227) {
        return try await settle_outer_throws() catch {
            return base + 20;
        };
    } else {
        var value = try await settle_resolves_after_delay() catch {
            return -1;
        };
        return base + value + 30;
    }
}

async func run_else_if_await_returns_cases() Promise {
    printf("elif tt={}\n", await do_else_if_await_returns(true, false, 5));
    printf("elif ft={}\n", await do_else_if_await_returns(false, true, 5));
    printf("elif ff={}\n", await do_else_if_await_returns(false, false, 5));
}

func test_branchy_returns() {
    println("=== Branchy returns ===");

    do_branchy_returns(true, true, 5).then(func (value: int) {
        printf("tt={}\n", value);
    });
    do_branchy_returns(true, false, 5).then(func (value: int) {
        printf("tf={}\n", value);
    });
    do_branchy_returns(false, true, 5).then(func (value: int) {
        printf("ft={}\n", value);
    });
    do_branchy_returns(false, false, 5).then(func (value: int) {
        printf("ff={}\n", value);
    });
}

func test_condition_await_returns() {
    println("=== Condition await returns ===");
    run_condition_await_returns_cases();
}

func test_else_if_await_returns() {
    println("=== Else if await returns ===");
    run_else_if_await_returns_cases();
}

// --- Await: while loop control flow ---

async func do_while_plain(limit: int) Promise<int> {
    var i = 0;
    var sum = 0;

    while i < limit {
        sum = sum + await delayed_number(1);
        i = i + 1;
    }

    return sum;
}

async func do_while_awaited_cond(limit: int) Promise<int> {
    var i = 0;
    var sum = 0;

    while await delayed_flag(i < limit, 228) {
        sum = sum + await delayed_number(1);
        i = i + 1;
    }

    return sum;
}

async func do_while_await_return(limit: int) Promise<int> {
    var i = 0;
    var sum = 0;

    while await delayed_flag(i < limit, 229) {
        sum = sum + await delayed_number(1);
        if await delayed_flag(i == 1, 230) {
            return sum + 100;
        }
        i = i + 1;
    }

    return sum;
}

async func do_while_try_await(limit: int) Promise<int> {
    var i = 0;
    var sum = 0;

    while await delayed_flag(i < limit, 231) {
        sum = sum + try await settle_resolves_after_delay() catch {
            return -1000;
        };
        i = i + 1;
    }

    return sum;
}

async func run_while_await_cases() Promise {
    printf("while plain={}\n", await do_while_plain(3));
    printf("while awaited={}\n", await do_while_awaited_cond(3));
    printf("while return={}\n", await do_while_await_return(3));
    printf("while try={}\n", await do_while_try_await(2));
}

func test_while_await() {
    println("=== While await ===");
    run_while_await_cases();
}

// --- Await: while break / continue ---

async func do_while_break(limit: int) Promise<int> {
    var i = 0;
    var sum = 0;

    while await delayed_flag(i < limit, 232) {
        sum = sum + await delayed_number(1);
        if await delayed_flag(i == 1, 233) {
            break;
        }
        i = i + 1;
    }

    return sum + i;
}

async func do_while_continue(limit: int) Promise<int> {
    var i = 0;
    var sum = 0;

    while await delayed_flag(i < limit, 234) {
        i = i + 1;
        if await delayed_flag(i == 2, 235) {
            continue;
        }
        sum = sum + await delayed_number(i);
    }

    return sum;
}

func test_while_loop_control() {
    println("=== While loop control ===");

    do_while_break(5).then(func (value: int) {
        printf("while break={}\n", value);
    });

    do_while_continue(4).then(func (value: int) {
        printf("while continue={}\n", value);
    });
}

// --- Await: for loop control flow ---

struct AsyncIterNode<T: ops.Construct> {
    value: T = T{};
    next: ?*AsyncIterNode<T> = null;
}

struct AsyncLinkedListIterator<T: ops.Construct> {
    current: ?*AsyncIterNode<T> = null;

    impl ops.MutIterator<T> {
        mut func next() ?(&mut T) {
            if !this.current {
                return null;
            }
            var ptr = this.current;
            this.current = ptr.next;
            unsafe {
                return &mut ptr.value;
            }
        }
    }
}

struct AsyncLinkedList<T: ops.Construct> {
    head: ?*AsyncIterNode<T> = null;

    impl ops.MutIterable<T> {
        func to_iter_mut() AsyncLinkedListIterator<T> {
            return {current: this.head};
        }
    }
}

async func do_for_plain(limit: int) Promise<int> {
    var sum = 0;

    for i in 0..limit {
        sum = sum + await delayed_number(i + 1);
    }

    return sum;
}

async func do_for_control(limit: int) Promise<int> {
    var sum = 0;

    for i in 0..limit {
        if await delayed_flag(i == 2, 236) {
            continue;
        }
        sum = sum + await double_it(i + 1);
        if await delayed_flag(i == 3, 237) {
            break;
        }
    }

    return sum;
}

async func do_for_try_await(limit: int) Promise<int> {
    var sum = 0;

    for i in 0..limit {
        sum = sum + try await settle_resolves_after_delay() catch {
            return -1000;
        };
        if await delayed_flag(i == 1, 238) {
            break;
        }
    }

    return sum;
}

async func run_for_await_cases() Promise {
    printf("for plain={}\n", await do_for_plain(4));
    printf("for control={}\n", await do_for_control(6));
    printf("for try={}\n", await do_for_try_await(4));
}

func test_for_await() {
    println("=== For await ===");
    run_for_await_cases();
}

async func do_iter_for_plain(list: AsyncLinkedList<int>) Promise<int> {
    var sum = 0;

    for item in list {
        sum = sum + await delayed_number(*item * 10);
    }

    return sum;
}

async func do_iter_for_mutate_first(list: AsyncLinkedList<int>) Promise<int> {
    for item in list {
        var inc = await delayed_number(10);
        *item = *item + inc;
        return *item;
    }

    return -1;
}

async func do_iter_for_control(list: AsyncLinkedList<int>) Promise<int> {
    var sum = 0;

    for item in list {
        if await delayed_flag(*item == 2, 239) {
            continue;
        }
        sum = sum + await double_it(*item);
        if await delayed_flag(*item == 3, 240) {
            break;
        }
    }

    return sum;
}

async func do_iter_for_indexed(list: AsyncLinkedList<int>) Promise<int> {
    var sum = 0;

    for item, i in list {
        sum = sum + await delayed_number(*item + i);
    }

    return sum;
}

async func do_iter_for_try(list: AsyncLinkedList<int>) Promise<int> {
    var sum = 0;

    for item in list {
        if await delayed_flag(*item == 2, 241) {
            sum = sum + try await settle_outer_throws() catch {
                500
            };
        }
        sum = sum + try await delayed_number(*item * 10) catch {
            return -1000;
        };
        if await delayed_flag(*item == 3, 242) {
            break;
        }
    }

    return sum;
}

async func run_iter_for_await_cases() Promise {
    var m2 = new AsyncIterNode<int>{value: 2};
    var m1 = new AsyncIterNode<int>{value: 1, next: m2};
    var mutate_list = AsyncLinkedList<int>{head: m1};

    var n4 = new AsyncIterNode<int>{value: 4};
    var n3 = new AsyncIterNode<int>{value: 3, next: n4};
    var n2 = new AsyncIterNode<int>{value: 2, next: n3};
    var n1 = new AsyncIterNode<int>{value: 1, next: n2};
    var list = AsyncLinkedList<int>{head: n1};

    printf("iter mutate={}\n", await do_iter_for_mutate_first(mutate_list));
    printf("iter plain={}\n", await do_iter_for_plain(list));
    printf("iter control={}\n", await do_iter_for_control(list));
    printf("iter indexed={}\n", await do_iter_for_indexed(list));
    printf("iter try={}\n", await do_iter_for_try(list));

    unsafe {
        delete m1;
        delete m2;
        delete n1;
        delete n2;
        delete n3;
        delete n4;
    }
}

func test_iter_for_await() {
    println("=== Iterator for await ===");
    run_iter_for_await_cases();
}

// --- Await: bare then logic ---

async func do_bare_then_logic() Promise {
    var x = 10;
    await time.sleep(1);
    x = x + 5;
    printf("x = {}\n", x);
    await time.sleep(1);
    printf("x still = {}\n", x);
}

func test_bare_then_logic() {
    println("=== Bare then logic ===");
    do_bare_then_logic();
}

// --- Await: multiple bare ---

async func do_multiple_bare() Promise {
    await time.sleep(1);
    println("after first");
    await time.sleep(1);
    println("after second");
    await time.sleep(1);
    println("after third");
}

func test_multiple_bare() {
    println("=== Multiple bare ===");
    do_multiple_bare();
}

// --- Promise reject ---

struct TestError {
    code: int = 0;

    impl Error {
        func message() string {
            return stringf("error {}", this.code);
        }
    }
}

struct OtherTestError {
    code: int = 0;

    impl Error {
        func message() string {
            return stringf("other {}", this.code);
        }
    }
}

struct TraceAsyncError {
    code: int = 0;

    mut func new(code: int = 0) {
        this.code = code;
        if code != 0 {
            printf("TraceAsyncError.new({})\n", code);
        }
    }

    mut func delete() {
        if this.code != 0 {
            printf("TraceAsyncError.delete({})\n", this.code);
        }
    }

    impl ops.Copy {
        mut func copy(source: &This) {
            this.code = source.code;
            if source.code != 0 {
                printf("TraceAsyncError.copy({})\n", source.code);
            }
        }
    }

    impl Error {
        func message() string {
            return stringf("trace {}", this.code);
        }
    }
}

struct TraceAsyncValue {
    value: int = 0;

    mut func delete() {
        if this.value != 0 {
            printf("TraceAsyncValue.delete({})\n", this.value);
        }
    }

    impl ops.Copy {
        mut func copy(source: &This) {
            this.value = source.value;
            if source.value != 0 {
                printf("TraceAsyncValue.copy({})\n", source.value);
            }
        }
    }
}

func test_reject() {
    println("=== reject ===");
    var p = Promise<int>{};
    p.reject(new TestError{code: 42});
    printf("is_resolved: {}\n", p.is_resolved());
    printf("is_rejected: {}\n", p.is_rejected());
}

func test_reject_chain() {
    println("=== reject chain ===");
    var p = Promise<int>{};
    var p2 = p.then(func (v: int) string {
        return stringf("v={}", v);
    });
    p.reject(new TestError{code: 99});
    printf("p rejected: {}\n", p.is_rejected());
    printf("p2 rejected: {}\n", p2.is_rejected());
}

func test_settle() {
    println("=== settle ===");

    settle_resolves_after_delay().settle(
        func (result) {
            printf("settle ok value: {}\n", result.value()!);
            printf("settle ok error null: {}\n", result.error() == null);
        }
    );

    settle_outer_throws().settle(
        func (result) {
            printf("settle err value null: {}\n", result.value() == null);
            printf("settle err message: {}\n", result.error()!.message());
        }
    );
}

func test_reject_after_resolve() {
    println("=== reject after resolve ===");
    var p = Promise<int>{};
    p.resolve(10);
    p.reject(new TestError{code: 1});
    printf("is_resolved: {}\n", p.is_resolved());
    printf("is_rejected: {}\n", p.is_rejected());
    printf("value: {}\n", p.value()!);
}

func test_resolve_after_reject() {
    println("=== resolve after reject ===");
    var p = Promise<int>{};
    p.reject(new TestError{code: 2});
    p.resolve(10);
    printf("is_resolved: {}\n", p.is_resolved());
    printf("is_rejected: {}\n", p.is_rejected());
}

// --- Async throw → rejection ---

func might_throw(should_throw: bool) int {
    if should_throw {
        throw new TestError{code: 42};
    }
    return 10;
}

// immediate throw (no await)
async func async_throw_immediate() Promise<int> {
    throw new TestError{code: 10};
    return 0;
}

// throw after delay
async func async_throw_after_delay() Promise<int> {
    var y = await time.sleep(50);
    throw new TestError{code: 20};
    return 0;
}

// called fn throws after delay
async func async_called_throws_after_delay() Promise<int> {
    var y = await time.sleep(100);
    var x = might_throw(true);
    return x;
}

// normal resolve after delay
async func async_resolves_after_delay() Promise<int> {
    var before = 42;
    var y = await time.sleep(150);
    return before;
}

// resolve after delay for settle()
async func settle_resolves_after_delay() Promise<int> {
    var y = await time.sleep(125);
    return 55;
}

// nested throw after delay for settle()
async func settle_inner_throws() Promise<int> {
    var y = await time.sleep(175);
    throw new TestError{code: 66};
    return 0;
}

async func settle_outer_throws() Promise<int> {
    var x = await settle_inner_throws();
    return x + 1;
}

async func settle_typed_throws() Promise<int> {
    var y = await time.sleep(185);
    throw new TestError{code: 67};
    return 0;
}

async func settle_other_throws() Promise<int> {
    var y = await time.sleep(195);
    throw new OtherTestError{code: 77};
    return 0;
}

async func trace_async_throw_after_delay() Promise<int> {
    var y = await time.sleep(205);
    throw new TraceAsyncError{code: 30};
    return 0;
}

async func trace_async_value_after_delay(value: int) Promise<TraceAsyncValue> {
    var y = await time.sleep(215);
    return TraceAsyncValue{value: value};
}

async func trace_async_lifecycle_probe() Promise<int> {
    if await delayed_flag(false, 210) {
        return 99;
    }

    var result = try await trace_async_throw_after_delay() catch TraceAsyncError as err {
        printf("trace caught: {}\n", err.message());
        return 1;
    };

    return result;
}

async func trace_async_value_loop() Promise<int> {
    var sum = 0;
    var i = 0;
    while i < 2 {
        var value = await trace_async_value_after_delay(i + 1);
        printf("TraceAsyncValue.got({})\n", value.value);
        sum = sum + value.value;
        i = i + 1;
    }
    return sum;
}

func add_one_sync(x: int) int {
    return x + 1;
}

// --- Await: switch control flow ---

async func switch_expr_await(tag: int, flip: bool) Promise<int> {
    return switch await double_it(tag) {
        2 => if await double_it(if flip { 50 } else { 49 }) == 100 { 100 } else { 101 },
        4 => add_one_sync(await double_it(10)),
        6 => try add_one_sync(await async_throw_immediate()) catch { -7 },
        else => switch await double_it(if flip { 2 } else { 3 }) {
            4 => 400,
            else => 500,
        },
    };
}

async func switch_stmt_await(tag: int, flip: bool) Promise<int> {
    switch await double_it(tag) {
        2 => {
            if await double_it(if flip { 1 } else { 0 }) == 2 {
                return 10;
            }
            return 11;
        },
        4 => {
            var x = await double_it(10);
            return x + 2;
        },
        6 => {
            return try add_one_sync(await async_throw_immediate()) catch { -30 };
        },
        else => {
            if await double_it(if flip { 2 } else { 3 }) == 4 {
                return 40;
            }
            return 50;
        }
    }
}

async func run_switch_await_cases() Promise {
    printf("expr1t={}\n", await switch_expr_await(1, true));
    printf("expr1f={}\n", await switch_expr_await(1, false));
    printf("expr2={}\n", await switch_expr_await(2, false));
    printf("expr3={}\n", await switch_expr_await(3, false));
    printf("expr9t={}\n", await switch_expr_await(9, true));
    printf("expr9f={}\n", await switch_expr_await(9, false));
    printf("stmt1t={}\n", await switch_stmt_await(1, true));
    printf("stmt1f={}\n", await switch_stmt_await(1, false));
    printf("stmt2={}\n", await switch_stmt_await(2, false));
    printf("stmt3={}\n", await switch_stmt_await(3, false));
    printf("stmt9t={}\n", await switch_stmt_await(9, true));
    printf("stmt9f={}\n", await switch_stmt_await(9, false));
}

func test_switch_await() {
    println("=== Switch await ===");
    run_switch_await_cases();
}

// --- Await: type switch control flow ---

interface AsyncShape {}

struct AsyncCircle {
    radius: int = 0;
    impl AsyncShape {}
}

struct AsyncRect {
    w: int = 0;
    h: int = 0;
    impl AsyncShape {}
}

async func async_make_shape(flag: bool) Promise<&AsyncShape> {
    if flag {
        return new AsyncCircle{radius: 7};
    }
    return new AsyncRect{w: 3, h: 4};
}

async func type_switch_stmt_await(flag: bool) Promise<int> {
    var s = await async_make_shape(flag);
    switch s.(type) {
        &AsyncCircle => {
            return await double_it(s.radius + 1);
        },
        &AsyncRect => {
            return await double_it(s.w + s.h + 2);
        },
        else => {
            return 99;
        }
    }
}

async func type_switch_expr_await(flag: bool) Promise<int> {
    var s = await async_make_shape(flag);
    return switch s.(type) {
        &AsyncCircle => await double_it(s.radius + 1),
        &AsyncRect => await double_it(s.w + s.h + 2),
        else => 88,
    };
}

async func type_switch_try_await(flag: bool) Promise<int> {
    var s = await async_make_shape(flag);
    switch s.(type) {
        &AsyncCircle => {
            return s.radius + try await settle_resolves_after_delay() catch {
                return -100;
            };
        },
        &AsyncRect => {
            return s.w + s.h + try await settle_outer_throws() catch {
                return -200;
            };
        },
        else => {
            return 99;
        }
    }
}

async func run_type_switch_await_cases() Promise {
    printf("type stmt circle={}\n", await type_switch_stmt_await(true));
    printf("type stmt rect={}\n", await type_switch_stmt_await(false));
    printf("type expr circle={}\n", await type_switch_expr_await(true));
    printf("type expr rect={}\n", await type_switch_expr_await(false));
    printf("type try circle={}\n", await type_switch_try_await(true));
    printf("type try rect={}\n", await type_switch_try_await(false));
}

func test_type_switch_await() {
    println("=== Type switch await ===");
    run_type_switch_await_cases();
}

async func try_await_result_ok() Promise<Result<int, Shared<Error>>> {
    return try await settle_resolves_after_delay();
}

async func try_await_result_err() Promise<Result<int, Shared<Error>>> {
    return try await settle_outer_throws();
}

async func try_await_catch_ok() Promise<int> {
    var value = try await settle_resolves_after_delay() catch {
        return -1;
    };
    return value + 2;
}

async func try_await_catch_err() Promise<int> {
    var value = try await settle_outer_throws() catch {
        return -7;
    };
    return value;
}

async func try_await_typed_result_err() Promise<Result<int, Shared<Error>>> {
    return try await settle_typed_throws() catch TestError;
}

async func try_await_typed_result_mismatch() Promise<Result<int, Shared<Error>>> {
    return try await settle_other_throws() catch TestError;
}

async func try_await_typed_catch_err() Promise<int> {
    var value = try await settle_typed_throws() catch TestError as err {
        printf("try await typed catch saw: {}\n", err.message());
        return -17;
    };
    return value;
}

async func try_await_typed_catch_mismatch() Promise<int> {
    var value = try await settle_other_throws() catch TestError as err {
        return -999;
    };
    return value;
}

async func try_await_nested_result_ok() Promise<Result<int, Shared<Error>>> {
    return try add_one_sync(await settle_resolves_after_delay());
}

async func try_await_nested_result_err() Promise<Result<int, Shared<Error>>> {
    return try add_one_sync(await settle_outer_throws());
}

async func try_await_nested_catch_err() Promise<int> {
    var value = try add_one_sync(await settle_outer_throws()) catch {
        return -27;
    };
    return value;
}

async func try_await_branchy(flag1: bool, flag2: bool) Promise<int> {
    if await delayed_flag(flag1, 208) {
        var a = try await settle_resolves_after_delay() catch {
            return -101;
        };

        if await delayed_flag(flag2, 209) {
            return a + 1000;
        }

        return try add_one_sync(await settle_resolves_after_delay()) catch {
            return -102;
        };
    }

    if await delayed_flag(flag2, 210) {
        return try await settle_outer_throws() catch {
            return -300;
        };
    }

    return try add_one_sync(await settle_outer_throws()) catch {
        return -400;
    };
}

async func try_await_pathological(flag1: bool, flag2: bool, flag3: bool, flag4: bool,
                                  base: int) Promise<int> {
    var root = await delayed_number(base);

    if await delayed_flag(flag1, 211) {
        if await delayed_flag(flag2, 212) {
            if await delayed_flag(flag3, 213) {
                if await delayed_flag(flag4, 214) {
                    var a = try await settle_resolves_after_delay() catch {
                        return -1001;
                    };
                    return root + a + 10000;
                }

                return try add_one_sync(await settle_outer_throws()) catch {
                    return root + 1001;
                };
            }

            if await delayed_flag(flag4, 215) {
                return try await settle_typed_throws() catch TestError as err {
                    return root + err.code + 2000;
                };
            }

            return try await settle_other_throws() catch {
                return root + 2001;
            };
        }

        if await delayed_flag(flag3, 216) {
            var b = await delayed_number(30);
            if await delayed_flag(flag4, 217) {
                return root + b + 3000;
            }

            var c = try await settle_resolves_after_delay() catch {
                return -3001;
            };
            return root + b + c + 3001;
        }

        if await delayed_flag(flag4, 225) {
            return root + 3002 + await delayed_number(31);
        }

        return root + 3003 + await delayed_number(32);
    }

    if await delayed_flag(flag2, 218) {
        if await delayed_flag(flag3, 219) {
            var d = try add_one_sync(await settle_resolves_after_delay()) catch {
                return -4001;
            };
            if await delayed_flag(flag4, 220) {
                return root + d + 4000;
            }
            return root + d + 4001;
        }

        if await delayed_flag(flag4, 221) {
            return try await settle_outer_throws() catch {
                return root + 4002;
            };
        }

        return root + await delayed_number(32) + 4003;
    }

    if await delayed_flag(flag3, 222) {
        if await delayed_flag(flag4, 223) {
            var e = try await settle_typed_throws() catch TestError as err {
                return root + err.code + 5000;
            };
            return e;
        }

        return root + await delayed_number(33) +
               try await settle_resolves_after_delay() catch {
                   return -5001;
               };
    }

    if await delayed_flag(flag4, 224) {
        return root + 5002;
    }

    return root + await delayed_number(34) + 5003;
}

async func run_try_await_pathological_cases() Promise {
    printf("path 1111={}\n", await try_await_pathological(true, true, true, true, 5));
    printf("path 1110={}\n", await try_await_pathological(true, true, true, false, 5));
    printf("path 1101={}\n", await try_await_pathological(true, true, false, true, 5));
    printf("path 1100={}\n", await try_await_pathological(true, true, false, false, 5));
    printf("path 1011={}\n", await try_await_pathological(true, false, true, true, 5));
    printf("path 1010={}\n", await try_await_pathological(true, false, true, false, 5));
    printf("path 1001={}\n", await try_await_pathological(true, false, false, true, 5));
    printf("path 1000={}\n", await try_await_pathological(true, false, false, false, 5));
    printf("path 0111={}\n", await try_await_pathological(false, true, true, true, 5));
    printf("path 0110={}\n", await try_await_pathological(false, true, true, false, 5));
    printf("path 0101={}\n", await try_await_pathological(false, true, false, true, 5));
    printf("path 0100={}\n", await try_await_pathological(false, true, false, false, 5));
    printf("path 0011={}\n", await try_await_pathological(false, false, true, true, 5));
    printf("path 0010={}\n", await try_await_pathological(false, false, true, false, 5));
    printf("path 0001={}\n", await try_await_pathological(false, false, false, true, 5));
    printf("path 0000={}\n", await try_await_pathological(false, false, false, false, 5));
}

func test_try_await() {
    println("=== try await ===");

    try_await_result_ok().then(
        func (result) Unit {
            printf("try await ok: {}\n", result.value()!);
            return {};
        }
    );

    try_await_result_err().then(
        func (result) Unit {
            printf("try await err: {}\n", result.error()!.message());
            return {};
        }
    );

    try_await_catch_ok().then(
        func (value: int) Unit {
            printf("try await catch ok: {}\n", value);
            return {};
        }
    );

    try_await_catch_err().then(
        func (value: int) Unit {
            printf("try await catch err: {}\n", value);
            return {};
        }
    );

    try_await_typed_result_err().then(
        func (result) Unit {
            printf("try await typed err: {}\n", result.error()!.message());
            return {};
        }
    );

    try_await_typed_result_mismatch().catch(
        func (err: Shared<Error>) Result<int, Shared<Error>> {
            printf("try await typed mismatch: {}\n", err.message());
            return Result<int, Shared<Error>>.Ok{-1};
        }
    );

    try_await_typed_catch_err().then(
        func (value: int) Unit {
            printf("try await typed catch err: {}\n", value);
            return {};
        }
    );

    try_await_typed_catch_mismatch().catch(
        func (err: Shared<Error>) int {
            printf("try await typed catch mismatch: {}\n", err.message());
            return -1;
        }
    );

    try_await_nested_result_ok().then(
        func (result) Unit {
            printf("try await nested ok: {}\n", result.value()!);
            return {};
        }
    );

    try_await_nested_result_err().then(
        func (result) Unit {
            printf("try await nested err: {}\n", result.error()!.message());
            return {};
        }
    );

    try_await_nested_catch_err().then(
        func (value: int) Unit {
            printf("try await nested catch err: {}\n", value);
            return {};
        }
    );

    try_await_branchy(true, true).then(
        func (value: int) Unit {
            printf("try branch tt={}\n", value);
            return {};
        }
    );

    try_await_branchy(true, false).then(
        func (value: int) Unit {
            printf("try branch tf={}\n", value);
            return {};
        }
    );

    try_await_branchy(false, true).then(
        func (value: int) Unit {
            printf("try branch ft={}\n", value);
            return {};
        }
    );

    try_await_branchy(false, false).then(
        func (value: int) Unit {
            printf("try branch ff={}\n", value);
            return {};
        }
    );

    run_try_await_pathological_cases();

    trace_async_lifecycle_probe().then(
        func (value: int) Unit {
            printf("trace lifecycle={}\n", value);
            return {};
        }
    );

    trace_async_value_loop().then(
        func (value: int) Unit {
            printf("trace loop={}\n", value);
            return {};
        }
    );

}

func test_async_throw_immediate() {
    println("=== async throw immediate ===");
    var p = async_throw_immediate();
    printf("rejected: {}\n", p.is_rejected());
    p.catch(
        func (err: Shared<Error>) int {
            printf("caught: {}\n", err.message());
            return -1;
        }
    );
}

func test_async_throw_after_delay() {
    println("=== async throw after delay ===");
    var p = async_throw_after_delay();
    printf("immediate rejected: {}\n", p.is_rejected());
    p.catch(
        func (err: Shared<Error>) int {
            printf("delayed caught: {}\n", err.message());
            return -1;
        }
    );
}

func test_async_called_throws() {
    println("=== async called throws ===");
    var p = async_called_throws_after_delay();
    p.catch(
        func (err: Shared<Error>) int {
            printf("called caught: {}\n", err.message());
            return -1;
        }
    );
}

func test_async_resolve_after_delay() {
    println("=== async resolve after delay ===");
    var p = async_resolves_after_delay();
    p.then(func (v: int) {
        printf("resolved: {}\n", v);
    });
}

// --- Nested rejection propagation ---

async func nested_inner_throws() Promise<int> {
    throw new TestError{code: 77};
    return 0;
}

async func nested_outer_immediate() Promise<int> {
    var x = await nested_inner_throws();
    return x + 1;
}

async func nested_inner_delayed() Promise<int> {
    var y = await time.sleep(200);
    throw new TestError{code: 88};
    return 0;
}

async func nested_outer_delayed() Promise<int> {
    var x = await nested_inner_delayed();
    return x + 1;
}

async func nested_3level_bottom() Promise<int> {
    var y = await time.sleep(250);
    throw new TestError{code: 33};
    return 0;
}

async func nested_3level_middle() Promise<int> {
    var x = await nested_3level_bottom();
    return x + 1;
}

async func nested_3level_top() Promise<int> {
    var x = await nested_3level_middle();
    return x + 1;
}

func test_nested_rejection_immediate() {
    println("=== nested rejection immediate ===");
    var p = nested_outer_immediate();
    printf("rejected: {}\n", p.is_rejected());
    p.catch(
        func (err: Shared<Error>) int {
            printf("nested caught: {}\n", err.message());
            return -1;
        }
    );
}

func test_nested_rejection_delayed() {
    println("=== nested rejection delayed ===");
    var p = nested_outer_delayed();
    p.catch(
        func (err: Shared<Error>) int {
            printf("nested delayed caught: {}\n", err.message());
            return -1;
        }
    );
}

func test_nested_rejection_3level() {
    println("=== nested rejection 3level ===");
    var p = nested_3level_top();
    p.catch(
        func (err: Shared<Error>) int {
            printf("3level caught: {}\n", err.message());
            return -1;
        }
    );
}

// --- timeout / sleep ---

func test_timeout() {
    println("=== timeout ===");
    time.timeout(10, func () {
        println("timeout fired");
    });
    println("scheduled");
}

func test_sleep() {
    println("=== sleep ===");
    time.sleep(10).then(func (u) {
        println("sleep resolved");
    });
    println("scheduled");
}

func test_sleep_capture() {
    println("=== sleep capture ===");
    var counter = 42;
    time.sleep(10).then(func [counter] (u) {
        printf("captured: {}\n", counter);
    });
    counter = 999;
    printf("mutated: {}\n", counter);
}

func main() {
    // Promise API
    test_manual_promise();
    test_promise_state();
    test_then_chain();
    test_promise_make();
    test_promise_string();
    test_promise_in_struct();

    // Async no await
    test_async_no_await();

    // Await patterns (sync-resolved)
    test_var_decl();
    test_await_in_func_arg();
    test_await_in_binop();
    test_multiple_awaits();
    test_locals_across();
    test_chain();
    test_nested_chain();
    test_return_await();
    test_await_comparison();
    test_interleaved();
    test_await_string();
    test_string_concat();
    test_await_last();
    test_double_chain();
    test_branchy_returns();
    test_condition_await_returns();
    test_else_if_await_returns();
    test_while_await();
    test_while_loop_control();
    test_for_await();
    test_iter_for_await();
    test_switch_await();
    test_type_switch_await();

    // Await patterns (async-resolved via event loop)
    test_bare_await();
    test_bare_then_logic();
    test_multiple_bare();

    // Reject
    test_reject();
    test_reject_chain();
    test_settle();
    test_try_await();
    test_reject_after_resolve();
    test_resolve_after_reject();

    // Async throw → rejection
    test_async_throw_immediate();
    test_async_throw_after_delay();
    test_async_called_throws();
    test_async_resolve_after_delay();

    // Nested rejection propagation
    test_nested_rejection_immediate();
    test_nested_rejection_delayed();
    test_nested_rejection_3level();

    // Timer
    test_timeout();
    test_sleep();
    test_sleep_capture();

    println("All tests passed!");
}
