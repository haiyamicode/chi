// Stress managed-mode escape analysis with more realistic transitive escape shapes.

struct GCThing {
    id: int;

    mut func new(id: int) {
        this.id = id;
    }
}

struct RefSlot {
    ref: &GCThing;

    mut func new(r: &GCThing) {
        this.ref = r;
    }
}

struct NestedHolder {
    slot: RefSlot;
}

struct CallbackHolder {
    cb: func () int;

    mut func new(cb: func () int) {
        this.cb = cb;
    }
}

struct RefPair {
    first: &GCThing;
    second: &GCThing;

    mut func new(first: &GCThing, second: &GCThing) {
        this.first = first;
        this.second = second;
    }
}

struct GenericTempHolder<T> {
    value: T;

    mut func set(value: T) {
        let tmp = value;
        this.value = tmp;
    }

    func get() T {
        let tmp = this.value;
        return tmp;
    }
}

struct GenericCtorHolder<T> {
    value: T;

    mut func new(value: T) {
        let tmp = value;
        this.value = tmp;
    }

    func get() T {
        let tmp = this.value;
        return tmp;
    }
}

struct RefCollections {
    refs: Array<&GCThing> = [];
    lookup: Map<int, &GCThing> = {};
}

struct DeepCollections {
    collections: RefCollections = {};
}

func make_returned_lambda() (func () int) {
    var obj = GCThing{10};
    return func () int {
        return obj.id;
    };
}

func make_nested_returned_lambda() (func () int) {
    var obj = GCThing{20};
    var outer = func () (func () int) {
        return func () int {
            return obj.id + 1;
        };
    };
    return outer();
}

func make_nested_holder() NestedHolder {
    var obj = GCThing{300};
    return {slot: RefSlot{&obj}};
}

func make_optional_slot(flag: bool) ?RefSlot {
    var obj = GCThing{400};
    if flag {
        return RefSlot{&obj};
    }
    return null;
}

func make_callback_holder() CallbackHolder {
    var obj = GCThing{500};
    return {func () int {
        return obj.id;
    }};
}

func make_branch_pair(flag: bool) RefPair {
    var left = GCThing{600};
    var right = GCThing{601};
    if flag {
        return {&left, &right};
    }
    return {&right, &left};
}

func make_generic_temp_holder() GenericTempHolder<&GCThing> {
    var obj = GCThing{800};
    var holder = GenericTempHolder<&GCThing>{};
    holder.set(&obj);
    return holder;
}

func make_generic_ctor_holder() GenericCtorHolder<&GCThing> {
    var obj = GCThing{900};
    return {&obj};
}

func make_deep_collections() DeepCollections {
    var result = DeepCollections{};
    for i in 0..3 {
        var obj = GCThing{1000 + i};
        result.collections.refs.push(&obj);
        result.collections.lookup.set(i, &obj);
    }
    return result;
}

func make_generic_array_holder() GenericCtorHolder<Array<&GCThing>> {
    var refs: Array<&GCThing> = [];
    for i in 0..3 {
        var obj = GCThing{1100 + i};
        refs.push(&obj);
    }
    return {refs};
}

func make_generic_map_holder() GenericTempHolder<Map<int, &GCThing>> {
    var refs = Map<int, &GCThing>{};
    for i in 0..3 {
        var obj = GCThing{1200 + i};
        refs.set(i, &obj);
    }
    var holder = GenericTempHolder<Map<int, &GCThing>>{};
    holder.set(refs);
    return holder;
}

func test_returned_lambda() {
    println("=== Returned lambda ===");
    var cb = make_returned_lambda();
    printf("callback value = {}\n", cb());
}

func test_nested_returned_lambda() {
    println("\n=== Nested returned lambda ===");
    var cb = make_nested_returned_lambda();
    printf("nested callback value = {}\n", cb());
}

func test_nested_holder() {
    println("\n=== Nested holder ===");
    var nested = make_nested_holder();
    printf("nested slot = {}\n", nested.slot.ref.id);
}

func test_optional_ref_slot() {
    println("\n=== Optional ref slot ===");
    var present = make_optional_slot(true);
    if let slot = present {
        printf("present slot = {}\n", slot.ref.id);
    }
    var absent = make_optional_slot(false);
    if absent {
        println("unexpected present");
    } else {
        println("absent slot = null");
    }
}

func test_callback_holder() {
    println("\n=== Callback holder ===");
    var holder = make_callback_holder();
    printf("holder callback = {}\n", holder.cb());
}

func test_branch_pair() {
    println("\n=== Branch pair ===");
    var left_first = make_branch_pair(true);
    printf("pair(true) = [{}, {}]\n", left_first.first.id, left_first.second.id);

    var right_first = make_branch_pair(false);
    printf("pair(false) = [{}, {}]\n", right_first.first.id, right_first.second.id);
}

func test_generic_temp_holder() {
    println("\n=== Generic temp holder ===");
    var holder = make_generic_temp_holder();
    printf("holder ref = {}\n", holder.get().id);
}

func test_generic_ctor_holder() {
    println("\n=== Generic ctor holder ===");
    var holder = make_generic_ctor_holder();
    printf("holder ref = {}\n", holder.get().id);
}

func test_deep_collections() {
    println("\n=== Deep collections ===");
    var nested = make_deep_collections();
    printf(
        "refs = [{}, {}, {}]\n",
        nested.collections.refs[0].id,
        nested.collections.refs[1].id,
        nested.collections.refs[2].id
    );
    printf(
        "map = [{}, {}, {}]\n",
        nested.collections.lookup[0].id,
        nested.collections.lookup[1].id,
        nested.collections.lookup[2].id
    );
}

func test_generic_array_holder() {
    println("\n=== Generic array holder ===");
    var holder = make_generic_array_holder();
    printf(
        "holder refs = [{}, {}, {}]\n",
        holder.get()[0].id,
        holder.get()[1].id,
        holder.get()[2].id
    );
}

func test_generic_map_holder() {
    println("\n=== Generic map holder ===");
    var holder = make_generic_map_holder();
    var lookup = holder.get();
    printf("holder map = [{}, {}, {}]\n", lookup[0].id, lookup[1].id, lookup[2].id);
}

func main() {
    println("=== Managed Escape Stress ===");
    test_returned_lambda();
    test_nested_returned_lambda();
    test_nested_holder();
    test_optional_ref_slot();
    test_callback_holder();
    test_branch_pair();
    test_generic_temp_holder();
    test_generic_ctor_holder();
    test_deep_collections();
    test_generic_array_holder();
    test_generic_map_holder();
    println("\nManaged escape stress complete");
}
