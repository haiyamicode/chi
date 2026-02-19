import "std/mem" as mem;

struct GenericBox<T> {
    value: int = 0;

    func new() {
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

    func new(v: T) {
        this.value = v;
    }
}

struct Wrapper<T> {
    data: Shared<Inner<T>>;

    func init(value: T) {
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

    func new(v: T) {
        this.ref_count = 1;
        this.value = v;
    }
}

struct RefHolder<T> {
    data: *DataHolder<T> = null;

    func new(value: T) {
        this.data = new DataHolder<T>{value};
    }

    func get() T {
        return this.data.value;
    }

    func delete() {
        delete this.data;
    }
}

struct Arr<T> {
    data: *T = null;
    size: uint32 = 0;
    capacity: uint32 = 0;

    func add(item: T) {
        if this.size >= this.capacity {
            var new_cap: uint32 = this.capacity * 2;
            if this.capacity == 0 { new_cap = 4; }
            var new_data = mem.malloc(new_cap * sizeof T) as *T;
            if this.data {
                mem.memset(new_data as *void, 0, new_cap * sizeof T);
                var i: uint32 = 0;
                while i < this.size {
                    new_data[i] = this.data[i];
                    i = i + 1;
                }
                mem.free(this.data as *void);
            }
            this.data = new_data;
            this.capacity = new_cap;
        }
        this.data[this.size] = item;
        this.size = this.size + 1;
    }

    func delete() {
        delete this.data;
    }

    func get(index: uint32) T {
        assert(index < this.size, "index out of bounds");
        return this.data[index];
    }
}

func main() {
    var box_stack = GenericBox<int>{};
    printf("box_stack.value={}\n", box_stack.value);
    var box_heap: *GenericBox<int> = new GenericBox<int>{};
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
}

