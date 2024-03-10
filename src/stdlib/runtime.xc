struct HashBytes {
  data &void;
  size uint32;
}

extern "C" {
  func cx_print(str string);
  func cx_printf(format string, values *void);
  func cx_array_new(dest *void);
  func cx_array_add(dest *void, size uint32) *void;
  func cx_print_any(value *void);
  func cx_print_number(value uint64);
  func cx_gc_alloc(size uint32, destructor *void) *void;
  func cx_malloc(size uint32, ignored *void) *void;
  func cx_free(address *void);
  func cx_memset(address *void, v int, n uint32);
  func cx_runtime_start(stack *void);
  func cx_runtime_stop();
  func cx_panic(message string);
  func cx_personality(...) int32;
  func cx_timeout(delay uint64, callback *void);
  func cx_call(fn *void);
  func cx_string_format(format string, values *void) string;
  func cx_hbytes(value *any) HashBytes;
  func cx_map_new() *void;
  func cx_map_delete(data *void);
  func cx_map_find(data *void, key *HashBytes) *void;
  func cx_map_add(data *void, key *HashBytes, value *void);
  func cx_map_remove(data *void, key *HashBytes);
}

func println(value any) {
  cx_print_any(&value);
  cx_print("\n");
}

func gc_alloc(size uint32) *void {
  return cx_gc_alloc(size, null);
}

func print_int(value uint64) {
  cx_print_number(value);
}

func printf(format string, ...values any) {
  cx_printf(format, &values);
}

func panic(message string) {
  cx_panic(message);
}

func timeout(delay uint64, callback func) {
  cx_timeout(delay, &callback);
}

func stringf(format string, ...values any) string {
  return cx_string_format(format, &values);
}

func assert(cond bool, message string) {
  if !cond {
    panic(stringf("assertion failed: {}", message));
  }
}

struct Array<T> {
  data *T;
	size uint32;
	capacity uint32;

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

  @[std.ops.Index]
	func index(index uint32) &T {
		assert(index < this.size, "index out of bounds");
		return &this.data[index];
	}

  @[std.iter.Begin]
  func begin() uint32 {
    return 0;
  }

  @[std.iter.Next]
  func next(index uint32) uint32 {
    return index + 1;
  }

  @[std.iter.End]
  func end() uint32 {
    return this.size;
  }
}

struct Map<K, V> {
  data *void;

  func new() {
    this.data = cx_map_new();
  }

  func delete() {
    cx_map_delete(this.data);
    this.data = null;
  }

  func remove(key K) {
    var k any = key;
    var h = cx_hbytes(&k);
    cx_map_remove(this.data, &h);
  }

  func find(key K) ?&V {
    var k any = key;
    var h = cx_hbytes(&k);
    var p = cx_map_find(this.data, &h) as *V;
    if p {
      return {p};
    }
    return null;
  }

  @[std.ops.Index]
  func index(key K) &V {
    var k any = key;
    var h = cx_hbytes(&k);
    var p = cx_map_find(this.data, &h) as *V;
    if !p {
      p = new V{};
      cx_map_add(this.data, &h, p);
    }
    return p;
  }
}