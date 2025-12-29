// Test generic struct constructor on heap allocation
struct GenericBox<T> {
	value: int = 0;

	func new() {
		this.value = 42;
	}
}

// Test optional type inside generic struct (regression test for ?T clone bug)
struct Container<T> {
	value: ?T = null;
}

// Test reference type inside generic struct
struct RefContainer<T> {
	ptr: *T = null;
}

// Test nested generics (regression test for variant member lookup bug)
struct Inner<T> {
	value: T = undefined;
}

struct Wrapper<T> {
	data: Refc<Inner<T>>;

	func init(value: T) {
		var inner: Inner<T> = {};
		inner.value = value;
		this.data = {inner};
	}

	func get_inner() &Inner<T> {
		return this.data.get();  // Calls method on nested generic type
	}
}

struct Arr<T> {
	data: *T = null;
	size: uint32 = 0;
	capacity: uint32 = 0;

	func new() {
		cx_array_new(this);
	}

	func add(item: T) {
		var ptr = cx_array_add(this, sizeof T) as *T;
		ptr! = item;
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
	// Test generic struct constructor on stack
	var box_stack: GenericBox<int> = {};
	printf("box_stack.value={}\n", box_stack.value);

	// Test generic struct constructor on heap
	var box_heap: *GenericBox<int> = new GenericBox<int>{};
	printf("box_heap.value={}\n", box_heap.value);
	delete box_heap;

	var a: Arr<int> = {};
	a.add(1);
	a.add(2);
	printf("a.size={}\n", a.size);
	printf("a=[{},{}]\n", a.get(0), a.get(1));
	a.add(3);
	printf("a.size={}\n", a.size);
	printf("a=[{},{},{}]\n", a.get(0), a.get(1), a.get(2));

	// Test optional type inside generic struct
	var c: Container<int> = {};
	printf("c.value={}\n", c.value);
	c.value! = 42;
	printf("c.value={}\n", c.value!);

	// Test with string type
	var cs: Container<string> = {};
	cs.value! = "hello";
	printf("cs.value={}\n", cs.value!);

	// Test nested generics - calling method on Refc<Inner<T>> from within Wrapper<T>
	var w: Wrapper<int> = {};
	w.init(123);
	printf("w.get_inner().value={}\n", w.get_inner().value);
}