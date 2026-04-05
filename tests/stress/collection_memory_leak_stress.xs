import "std/mem" as mem;
import "std/math" as math;
import "std/time" as time;

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

    // Randomize array sizes (20-60 elements)
    let size = math.random_int(20, 60);
    // Random batch size for nested operations (5-15)
    let batch_size = math.random_int(5, 15);

    var values: Array<MegaRecord> = [];
    var value_map = Map<int, MegaRecord>{};
    var shareds: Array<Shared<string>> = [];
    var shared_map = Map<int, Shared<string>>{};
    var texts: Array<string> = [];
    var text_map = Map<int, string>{};

    for slot in 0..size {
        let key = slot as int;

        values.push(make_mega_record(base, key));
        value_map.set(key, make_mega_record(base + 10000, key));
        // Random overwrite (30% chance)
        if math.random() < 0.3 {
            value_map.set(key, make_mega_record(base + 20000, key));
        }

        var shared = Shared<string>.from_value(make_text(base + 60000, key));
        shareds.push(shared);
        shared_map.set(key, shared);
        // Random extra reference then remove (20% chance)
        if math.random() < 0.2 {
            let extra_key = 1000 + key + math.random_int(0, 100);
            shared_map.set(extra_key, shared);
            if math.random() < 0.5 {
                shared_map.remove(extra_key);
            }
        }

        let text = make_text(base + 80000, key);
        texts.push(text);
        text_map.set(key, make_text(base + 81000, key));
        // Random alias (40% chance)
        if math.random() < 0.4 {
            text_map.set(key, text);
        }
    }

    var values_copy = values;
    var shareds_copy = shareds;
    var texts_copy = texts;

    // Randomly copy to negative key or not
    if math.random() < 0.7 {
        value_map.set(-1, values_copy[0]);
        shared_map.set(-1, shareds_copy[0]);
        text_map.set(-1, texts_copy[0]);
    }

    // Random removals
    let remove_idx1 = math.random_int(0, values.length);
    let remove_idx2 = math.random_int(0, shareds.length);
    let remove_idx3 = math.random_int(0, texts.length);
    value_map.remove(remove_idx1);
    shared_map.remove(remove_idx2);
    text_map.remove(remove_idx3);

    // Random batch nested operations
    for i in 0..batch_size {
        if math.random() < 0.5 {
            let idx = math.random_int(0, size);
            value_map.remove(idx);
        }
    }

    values.clear();
    shareds.clear();
    texts.clear();

    // Random final cleanup (50% chance)
    if math.random() < 0.5 {
        let cleanup_size = math.random_int(3, 10);
        for i in 0..cleanup_size {
            value_map.remove(i);
            shared_map.remove(i);
            text_map.remove(i);
        }
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
    // Seed RNG with current time for different test patterns each run
    math.random_seed(time.now());

    println("collection memory leak stress:");
    let checkpoint_every = 100;
    let checkpoint_count = 10;

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
    println("collection memory leak stress ok");
}
