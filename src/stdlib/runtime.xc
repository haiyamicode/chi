import "std/ops" as ops;

struct HashBytes {
  data: *void = null;
  size: uint32 = 0;
}

extern "C" {
  func cx_print(str: string);
  func cx_printf(format: *string, values: *void);
  func cx_array_new(dest: *void);
  func cx_array_delete(dest: *void);
  func cx_array_add(dest: *void, size: uint32) *void;
  func cx_array_write_str(dest: *void, str: *string);
  func cx_array_reserve(dest: *void, elem_size: uint32, new_cap: uint32);
  func cx_print_any(value: *void);
  func cx_print_number(value: uint64);
  func cx_print_string(str: *string);
  func cx_gc_alloc(size: uint32, destructor: *void) *void;
  func cx_malloc(size: uint32, ignored: *void) *void;
  func cx_free(address: *void);
  func cx_memset(address: *void, v: uint8, n: uint32);
  func cx_runtime_start(stack: *void);
  func cx_set_program_vtable(ptr: *void);
  func cx_runtime_stop();
  func cx_panic(message: *string);
  func cx_personality(...) int32;
  func cx_timeout(delay: uint64, callback: *void);
  func cx_call(fn: *void);
  func cx_string_format(format: *string, values: *void, str: *string);
  func cx_string_from_chars(data: *void, size: uint32, str: *string);
  func cx_string_delete(dest: *string);
  func cx_string_copy(dest: *string, src: *string);
  func cx_hbytes(value: *any, result: *HashBytes);
  func cx_map_new() *void;
  func cx_map_delete(data: *void);
  func cx_map_find(data: *void, key: *HashBytes) *void;
  func cx_map_add(data: *void, key: *HashBytes, value: *void);
  func cx_map_remove(data: *void, key: *HashBytes);

  func cx_parse_json(str: *string, result: *void);
  func cx_json_value_delete(data: *void);
  func cx_json_value_get(data: *void, key: *string, result: *void);
  func cx_json_value_convert(data: *void, kind: uint32, result: *void);
  func cx_json_array_index(data: *void, index: uint32, result: *void);
  func cx_json_array_length(data: *void) uint32;
  func cx_json_value_copy(data: *void, result: *void);

  func cx_file_read(path: *string, result: *string); 
}

enum JsonKind {
  Null,
  Bool,
  Int64,
  Uint64,
  Double,
  String,
  Array,
  Object
}

struct JsonValue implements ops.CopyFrom<JsonValue> {
  data: *void = null;
  kind: JsonKind = JsonKind.Null;

  func delete() {
    cx_json_value_delete(this.data);
  }

  func get(key: string) JsonValue {
    const new_value: JsonValue = {};
    cx_json_value_get(this.data, &key, &new_value);
    return new_value;
  }

  func assert_kind(kind: JsonKind) {
    if (this.kind != kind) {
      panic(stringf("expected {}, got {}",
        json_kind_display(kind),
        json_kind_display(this.kind)));
    }
  }

  func to_string() string {
    this.assert_kind(JsonKind.String);
    const result = "";
    cx_json_value_convert(this.data, JsonKind.String, &result);
    return result;
  }

  func to_bool() bool {
    this.assert_kind(JsonKind.Bool);
    const result = false;
    cx_json_value_convert(this.data, JsonKind.Bool, &result);
    return result;
  }

  func to_int() int64 {
    this.assert_kind(JsonKind.Int64);
    const result: int64 = 0;
    cx_json_value_convert(this.data, JsonKind.Int64, &result);
    return result;
  }

  func length() uint32 {
    return cx_json_array_length(this.data);
  }

  func to_array() Array<JsonValue> {
    const result: Array<JsonValue> = {};
    const len = this.length();
    for var i=0; i<len; i++ {
      const new_value: JsonValue = {};
      cx_json_array_index(this.data, i, &new_value);
      result.add(new_value);
    }
    return result;
  }

  func copy_from(from: &JsonValue) {
    cx_json_value_copy(from.data, this);
  }
}

func json_kind_display(kind: JsonKind) string {
  return switch kind {
    JsonKind.Null => "null",
    JsonKind.Bool => "bool",
    JsonKind.Int64 => "int64",
    JsonKind.Uint64 => "uint64",
    JsonKind.Double => "double",
    JsonKind.String => "string",
    JsonKind.Array => "array",
    JsonKind.Object => "object",
    else => "unknown",
  };
}

func println(value: any) {
  cx_print_any(&value);
  cx_print("\n");
}

func gc_alloc(size: uint32) *void {
  return cx_gc_alloc(size, null);
}

func print_int(value: uint64) {
  cx_print_number(value);
}

func printf(format: string, ...values: any) {
  cx_printf(&format, &values);
}

func panic(message: string) {
  cx_panic(&message);
}

func timeout(delay: uint64, callback: func) {
  cx_timeout(delay, &callback);
}

func stringf(format: string, ...values: any) string {
  var str: string = "";
  cx_string_format(&format, &values, &str);
  return str;
}

func assert(cond: bool, message: string) {
  if !cond {
    panic(stringf("assertion failed: {}", message));
  }
}

func json_parse(str: string) JsonValue {
  const result: JsonValue = {};
  cx_parse_json(&str, &result);
  return result;
}

func fs_read(path: string) string {
  var result: string = "";
  cx_file_read(&path, &result);
  return result;
}

struct Array<T> implements
  ops.Index<uint32, T>,
  ops.IndexIterable<uint32, T>,
  ops.CopyFrom<Array<T>>,
  ops.Display
{
  data: *T = null;
	len: uint32 = 0;
	capacity: uint32 = 0;

	func new() {
		cx_array_new(this);
	}

  func delete() {
		delete this.data;
	}

	func add(item: T) {
		var ptr = cx_array_add(this, sizeof T) as *T;
		*ptr = item;
	}

  func clear() {
    if (this.data) {
      delete this.data;
    }
    cx_array_new(this);
  }

	func index(index: uint32) &T {
		assert(index < this.len, "index out of bounds");
		return &this.data[index];
	}

  func begin() uint32 {
    return 0;
  }

  func end() uint32 {
    return this.len;
  }

  func next(index: uint32) uint32 {
    return index + 1;
  }

  func display() string {
    var buf: Buffer = {};
    buf.write("[");
    for this => item {
      buf.write(stringf("{}, ", item));
    }
    buf.write("]");
    return buf.to_string();
  }

  func copy_from(from: &Array<T>) {
    this.clear();
    for from => item {
      this.add(item);
    }
  }
}

struct Map<K, V> implements ops.Index<K, V> {
  data: *void;

  func new() {
    this.data = cx_map_new();
  }

  func delete() {
    cx_map_delete(this.data);
    this.data = null;
  }

  func remove(key: K) {
    var k: any = key;
    var h: HashBytes = {};
    cx_hbytes(&k, &h);
    cx_map_remove(this.data, &h);
  }

  func find(key: K) ?&V {
    var k: any = key;
    var h: HashBytes = {};
    cx_hbytes(&k, &h);
    var p = cx_map_find(this.data, &h) as *V;
    if p {
      return {p};
    }
    return null;
  }

  func index(key: K) &V {
    var k: any = key;
    var h: HashBytes = {};
    cx_hbytes(&k, &h);
    var p = cx_map_find(this.data, &h) as *V;
    if !p {
      p = new V{};
      cx_map_add(this.data, &h, p);
    }
    return p;
  }
}

struct Buffer {
  bytes: Array<char>;

  func new() {
    this.bytes = {};
  }

  func write(str: string) {
    cx_array_write_str(&this.bytes, &str);
  }

  func to_string() string {
    var str: string = "";
    cx_string_from_chars(this.bytes.data, this.bytes.len, &str);
    return str;
  }
}