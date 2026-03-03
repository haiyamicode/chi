import "std/mem" as mem;
import "std/ops" as ops;

struct GenericBox<T> {
    value: int = 0;

    mut func new() {
        this.value = 42;
    }
}

struct Container<T> {
    value: ?T = null;
}

struct RefContainer<T> {
    ptr: *T = null;
}

struct Inner<T> {
    value: T;

    mut func new(v: T) {
        this.value = v;
    }
}

struct Wrapper<T> {
    data: Shared<Inner<T>>;

    mut func init(value: T) {
        var inner = Inner<T>{value};
        this.data = {inner};
    }

    func get_inner() &Inner<T> {
        return this.data.as_ref();
    }
}

struct DataHolder<T> {
    ref_count: uint32;
    value: T;

    mut func new(v: T) {
        this.ref_count = 1;
        this.value = v;
    }
}

struct RefHolder<T> {
    data: *DataHolder<T> = null;

    mut func new(value: T) {
        this.data = new DataHolder<T>{value};
    }

    func get() T {
        return this.data.value;
    }

    mut func delete() {
        unsafe {
            delete this.data;
        }
    }

    impl ops.CopyFrom<RefHolder<T>> {
        mut func copy_from(source: &RefHolder<T>) {
            this.data = source.data;
        }
    }
}

struct Arr<T> {
    data: *T = null;
    size: uint32 = 0;
    capacity: uint32 = 0;

    mut func add(item: T) {
        unsafe {
            if this.size >= this.capacity {
                var new_cap: uint32 = this.capacity * 2;
                if this.capacity == 0 {
                    new_cap = 4;
                }
                var new_data = mem.malloc(new_cap * sizeof T) as *T;
                var i: uint32 = 0;
                while i < this.size {
                    new_data[i] = this.data[i];
                    i = i + 1;
                }
                delete this.data;
                this.data = new_data;
                this.capacity = new_cap;
            }
            this.data[this.size] = item;
            this.size = this.size + 1;
        }
    }

    mut func delete() {
        unsafe {
            delete this.data;
        }
    }

    impl ops.CopyFrom<Arr<T>> {
        mut func copy_from(source: &Arr<T>) {
            this.data = source.data;
            this.size = source.size;
            this.capacity = source.capacity;
        }
    }

    func get(index: uint32) T {
        assert(index < this.size, "index out of bounds");
        return this.data[index];
    }
}

// Generic function calling another generic function
func generic_identity<T>(v: T) T {
    return v;
}

func wrap_generic<T>(v: T) T {
    return generic_identity<T>(v);
}

func double_wrap_generic<T>(v: T) T {
    return wrap_generic<T>(v);
}

struct GenericCaller<T: ops.Construct> {
    val: T = {};

    func call_free_fn() T {
        return generic_identity<T>(this.val);
    }

    func call_chain() T {
        return wrap_generic<T>(this.val);
    }
}

func main() {
    var box_stack = GenericBox<int>{};
    printf("box_stack.value={}\n", box_stack.value);
    var box_heap = new GenericBox<int>{};
    printf("box_heap.value={}\n", box_heap.value);
    delete box_heap;
    var a = Arr<int>{};
    a.add(1);
    a.add(2);
    printf("a.size={}\n", a.size);
    printf("a=[{},{}]\n", a.get(0), a.get(1));
    a.add(3);
    printf("a.size={}\n", a.size);
    printf("a=[{},{},{}]\n", a.get(0), a.get(1), a.get(2));
    var c = Container<int>{};
    printf("c.value={}\n", c.value);
    c.value! = 42;
    printf("c.value={}\n", c.value!);
    var cs = Container<string>{};
    cs.value! = "hello";
    printf("cs.value={}\n", cs.value!);
    var w = Wrapper<int>{};
    w.init(123);
    printf("w.get_inner().value={}\n", w.get_inner().value);
    var rh = RefHolder<int>{999};
    printf("rh.get()={}\n", rh.get());

    // Generic function chaining
    printf("chain: {}\n", double_wrap_generic<int>(100));
    printf("chain_str: {}\n", double_wrap_generic<string>("chain"));
    var gc = GenericCaller<int>{val: 200};
    printf("struct_free: {}\n", gc.call_free_fn());
    printf("struct_chain: {}\n", gc.call_chain());
    var gcs = GenericCaller<string>{val: "method"};
    printf("struct_free_str: {}\n", gcs.call_free_fn());
}

