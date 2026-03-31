import "std/mem" as mem;
import "std/math" as math;
import "std/time" as time;

struct TestError {
    msg: string;
    code: int;

    impl Error {
        func message() string {
            return this.msg;
        }
    }
}

struct NestedError {
    inner: &TestError;

    impl Error {
        func message() string {
            return this.inner.message();
        }
    }
}

func make_payload(id: int, size: int) string {
    var result = stringf("payload:{}:0", id);
    for i in 1..size {
        result = result + "|" + stringf("payload:{}:{}", id, i);
    }
    return result;
}

struct ActionHolder {
    action: func () Unit;
}

func make_action_holder(name: string, should_throw: bool) ActionHolder {
    var data = make_payload(name.length * 1000, 5);
    return {
        action: func [data, should_throw] () {
            if should_throw {
                throw new TestError{msg: data, code: 200};
            }
        }
    };
}

func lambda_collection_stress() {
    var holders: Array<ActionHolder> = [];

    for i in 0..20 {
        let should_throw = math.random() < 0.3;
        holders.push(make_action_holder(stringf("holder:{}", i), should_throw));
    }

    for i in 0..holders.length {
        try (holders[i].action)() catch {};
    }
}

func assert_no_live_allocations(checkpoint: int) {
    let stats = mem.DebugAllocator.stats();
    assert(
        stats.live_bytes == 0,
        stringf("live bytes leaked at checkpoint {}: {}", checkpoint, stats.live_bytes)
    );
    assert(
        stats.live_alloc_count == 0,
        stringf("live allocations leaked at checkpoint {}: {}", checkpoint, stats.live_alloc_count)
    );
}

func main() {
    math.random_seed(time.now());

    println("lambda error recovery stress:");
    let checkpoint_every = 100;
    let checkpoint_count = 10;

    mem.DebugAllocator.set_enabled(true);
    mem.DebugAllocator.reset();

    for i in 0..(checkpoint_every * checkpoint_count) {
        lambda_collection_stress();
        if (i + 1) % checkpoint_every == 0 {
            let checkpoint = (i + 1) / checkpoint_every - 1;
            assert_no_live_allocations(checkpoint);
            mem.DebugAllocator.set_enabled(false);
            printf("checkpoint {}\n", checkpoint);
            mem.DebugAllocator.set_enabled(true);
            mem.DebugAllocator.reset();
        }
    }

    mem.DebugAllocator.set_enabled(false);
    println("lambda error recovery stress ok");
}
