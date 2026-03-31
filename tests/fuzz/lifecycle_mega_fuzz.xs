import "std/mem" as mem;

func make_text(seed: int, slot: int) string {
    return stringf(
        "value:{}:{}:{}:{}:{}:{}:{}:{}",
        seed,
        slot,
        seed + 1,
        slot + 1,
        seed + 2,
        slot + 2,
        seed + 3,
        slot + 3
    );
}

struct MegaRecord {
    title: string = "";
    subtitle: string = "";
    payload: string = "";
    numbers: Array<int> = [];
    labels: Array<string> = [];
    lookup: Map<int, string> = {};
}

func make_mega_record(seed: int, slot: int) MegaRecord {
    var record = MegaRecord{
        title: make_text(seed, slot),
        subtitle: make_text(seed + 1000, slot),
        payload: make_text(seed + 2000, slot) + make_text(seed + 3000, slot),
        numbers: [],
        labels: [],
        lookup: {}
    };
    for i in 0..64 {
        record.numbers.push(seed + slot * 100 + (i as int));
        record.labels.push(make_text(seed + 5000 + (i as int), slot));
        record.lookup.set(i as int, make_text(seed + 6000 + (i as int), slot));
    }
    return record;
}

func run_workload(iteration: int) {
    let base = iteration * 1000;

    var values: Array<MegaRecord> = [];
    var value_map = Map<int, MegaRecord>{};
    var shareds: Array<Shared<string>> = [];
    var shared_map = Map<int, Shared<string>>{};
    var texts: Array<string> = [];
    var text_map = Map<int, string>{};

    for slot in 0..40 {
        let key = slot as int;

        values.push(make_mega_record(base, key));
        value_map.set(key, make_mega_record(base + 10000, key));
        if slot % 3 == 0 {
            value_map.set(key, make_mega_record(base + 20000, key));
        }

        var shared = Shared<string>.from_value(make_text(base + 60000, key));
        shareds.push(shared);
        shared_map.set(key, shared);
        if slot % 5 == 0 {
            shared_map.set(1000 + key, shared);
            shared_map.remove(1000 + key);
        }

        let text = make_text(base + 80000, key);
        texts.push(text);
        text_map.set(key, make_text(base + 81000, key));
        if slot % 3 == 1 {
            text_map.set(key, text);
        }
    }

    var values_copy = values;
    var shareds_copy = shareds;
    var texts_copy = texts;

    value_map.set(-1, values_copy[0]);
    shared_map.set(-1, shareds_copy[0]);
    text_map.set(-1, texts_copy[0]);

    value_map.remove(0);
    shared_map.remove(2);
    text_map.remove(3);

    values.clear();
    shareds.clear();
    texts.clear();

    if iteration % 2 == 0 {
        value_map.remove(3);
        shared_map.remove(5);
        text_map.remove(6);
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
    println("mega lifecycle fuzz:");
    let checkpoint_every = 100 as int;
    let checkpoint_count = 10 as int;
    mem.DebugAllocator.set_enabled(true);
    mem.DebugAllocator.reset();
    for i in 0..(checkpoint_every * checkpoint_count) {
        run_workload(i as int);
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
    println("mega lifecycle fuzz ok");
}
