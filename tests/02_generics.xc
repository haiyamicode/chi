struct Arr<T> {
	data *T = null;
	size uint32 = 0;
	capacity uint32 = 0;

	func new() {
		cx_array_new(this);
	}

	func add(item T) {
		var ptr = cx_array_add(this, sizeof T) as *T;
		*ptr = item;
	}

	func delete() {
		delete this.data;
	}

	func get(index uint32) T {
		assert(index < this.size, "index out of bounds");
		return this.data[index];
	}
}

func main() {
	var a Arr<int> = {};
	a.add(1);
	a.add(2);
	printf("a.size={}\n", a.size);
	printf("a=[{},{}]\n", a.get(0), a.get(1));
	a.add(3);
	printf("a.size={}\n", a.size);
	printf("a=[{},{},{}]\n", a.get(0), a.get(1), a.get(2));
}