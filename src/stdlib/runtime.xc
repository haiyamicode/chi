import "std/ops" as ops;

private struct HashBytes {
    data: *void = null;
    length: uint32 = 0;
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
    func __copy_from(dest: *void, src: *void, destruct_old: bool);
    func cx_runtime_start(stack: *void);
    func cx_set_program_vtable(ptr: *void);
    func cx_runtime_stop();
    func cx_panic(message: *string);
    func cx_throw(type_info: *void, data_ptr: *void, vtable_ptr: *void, type_id: uint32);
    func cx_get_error_type_info() *void;
    func cx_get_error_data() *void;
    func cx_get_error_vtable() *void;
    func cx_get_error_type_id() uint32;
    func cx_personality(...) int32;
    func cx_timeout(delay: uint64, callback: *void);
    func cx_string_format(format: *string, values: *void, str: *string);
    func cx_string_from_chars(data: *void, size: uint32, str: *string);
    func cx_string_delete(dest: *string);
    func cx_string_copy(dest: *string, src: *string);
    func cx_string_to_cstring(str: *string) *char;
    func cx_string_concat(dest: *string, s1: *string, s2: *string);
    func cx_cstring_copy(src: *char) *char;
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
    func cx_debug(ptr: *void);
    func cx_capture_new(payload_size: uint32, captures_ti: *void, dtor: *void) *void;
    func cx_capture_retain(capture_ptr: *void);
    func cx_capture_release(capture_ptr: *void);
    func cx_capture_get_type(capture_ptr: *void) *void;
    func cx_capture_get_data(capture_ptr: *void) *void;
}

struct __CxEnumBase<T> implements ops.Display {
    private __value: T = undefined;
    private __display_name: *string = undefined;

    func display() string {
        var s = this.__display_name!;
        return string.format("{}", s);
    }

    func discriminator() T {
        return this.__value;
    }
}

private struct SharedData<T> {
    ref_count: uint32;
    value: T;

    func new(v: T) {
        this.ref_count = 1;
        this.value = v;
    }
}

struct Shared<T> implements ops.CopyFrom<Shared<T>>, ops.Display {
    private data: *SharedData<T> = null;

    func new(value: T) {
        this.data = new SharedData<T>{value};
    }

    private func retain() {
        if this.data {
            this.data.ref_count = this.data.ref_count + 1;
        }
    }

    private func release() {
        if this.data {
            var rc = this.data.ref_count - 1;
            this.data.ref_count = rc;
            if rc == 0 {
                delete this.data;
            }
        }
    }

    func delete() {
        this.release();
    }

    func copy_from(source: &Shared<T>) {
        source.retain();
        this.data = source.data;
    }

    func as_ref() &T {
        return &this.data.value;
    }

    func set(value: T) {
        if !this.data {
            this.data = new SharedData<T>{value};
        } else {
            this.data.value = value;
        }
    }

    func ref_count() uint32 {
        return this.data.ref_count;
    }

    func display() string {
        return string.display(this.as_ref());
    }
}

struct Box<T> implements ops.CopyFrom<Box<T>>, ops.Display {
    private _ptr: *T = null;

    func new(ptr: &mut<T>) {
        this._ptr = ptr as *T;
    }

    func from_ptr(ptr: *T) {
        if this._ptr {
            delete this._ptr;
        }
        this._ptr = ptr;
    }

    func delete() {
        if this._ptr {
            delete this._ptr;
        }
    }

    func copy_from(source: &Box<T>) {
        this._ptr = cx_malloc(sizeof source._ptr!, null) as *T;
        __copy_from(this._ptr, source._ptr, false);
    }

    func as_ref() &T {
        return this._ptr as &T;
    }

    func display() string {
        return string.display(this._ptr!);
    }
}

// Internal lambda struct for compiler-generated closures.
// Captures are type-erased (CxCapture payload pointer) so lambdas can be converted across
// capture types with the same call signature.
struct __CxLambda implements ops.CopyFrom<__CxLambda> {
    fn_ptr: *void = null;
    length: uint32 = 0;
    captures: *void = null; // CxCapture payload pointer (or null)

    func new(fn: *void, len: uint32) {
        this.fn_ptr = fn;
        this.length = len;
    }

    func set_captures_ptr(ptr: *void) {
        this.captures = ptr;
    }

    func delete() {
        if this.captures {
            cx_capture_release(this.captures);
            this.captures = null;
        }
    }

    func copy_from(source: &__CxLambda) {
        // Retain the source captures
        if source.captures {
            cx_capture_retain(source.captures);
        }

        // Copy all fields
        this.fn_ptr = source.fn_ptr;
        this.length = source.length;
        this.captures = source.captures;
    }

    func as_ptr() *void {
        return this.fn_ptr;
    }

    func data_ptr() *void {
        return cx_capture_get_data(this.captures);
    }
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
    private data: *void = null;
    protected kind: JsonKind = JsonKind.Null;

    func delete() {
        cx_json_value_delete(this.data);
    }

    func get(key: string) JsonValue {
        let new_value = JsonValue{};
        var key_ptr = &key;
        var value_ptr = &new_value;
        cx_json_value_get(this.data, key_ptr as *string, value_ptr as *void);
        return new_value;
    }

    func assert_kind(kind: JsonKind) {
        if this.kind != kind {
            panic(string.format("expected {}, got {}", json_kind_display(kind), json_kind_display(this.kind)));
        }
    }

    func to_string() string {
        this.assert_kind(JsonKind.String);
        let result = "";
        cx_json_value_convert(this.data, JsonKind.String.discriminator(), &result);
        return result;
    }

    func to_bool() bool {
        this.assert_kind(JsonKind.Bool);
        let result = false;
        cx_json_value_convert(this.data, JsonKind.Bool.discriminator(), &result);
        return result;
    }

    func to_int() int64 {
        this.assert_kind(JsonKind.Int64);
        let result: int64 = 0;
        cx_json_value_convert(this.data, JsonKind.Int64.discriminator(), &result);
        return result;
    }

    func length() uint32 {
        return cx_json_array_length(this.data);
    }

    func to_array() Array<JsonValue> {
        let result: Array<JsonValue> = [];
        let len = this.length();
        for var i = 0; i < len; i++ {
            let new_value = JsonValue{};
            cx_json_array_index(this.data, i, &new_value);
            result.add(new_value);
        }
        return result;
    }

    func copy_from(source: &JsonValue) {
        cx_json_value_copy(source.data, this);
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
        else => "unknown"
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
        panic(string.format("assertion failed: {}", message));
    }
}

func json_parse(str: string) JsonValue {
    let result = JsonValue{};
    cx_parse_json(&str, &result);
    return result;
}

func fs_read(path: string) string {
    var result: string = "";
    cx_file_read(&path, &result);
    return result;
}

struct Array<T> implements ops.Index<uint32, T>, ops.IndexIterable<uint32, T>, ops.CopyFrom<Array<T>>, ops.Display {
    private data: *T = null;
    protected length: uint32 = 0;
    protected capacity: uint32 = 0;

    func new(...values: T) {
        cx_array_new(this);
        if values.length > 0 {
            cx_array_reserve(this, sizeof T, values.length);
            for value in values {
                this.add(value);
            }
        }
    }

    func delete() {
        cx_free(this.data as *void);
    }

    func add(item: T) {
        var ptr = cx_array_add(this, sizeof T) as *T;
        ptr! = item;
    }

    func clear() {
        if this.data {
            cx_free(this.data as *void);
        }
        cx_array_new(this);
    }

    func index(index: uint32) &mut<T> {
        assert(index < this.length, "index out of bounds");
        return &mut this.data[index];
    }

    func begin() uint32 {
        return 0;
    }

    func end() uint32 {
        return this.length;
    }

    func next(index: uint32) uint32 {
        return index + 1;
    }

    func display() string {
        var buf = Buffer{};
        buf.write("[");
        for item in this {
            buf.write(string.format("{}, ", item));
        }
        buf.write("]");
        return buf.to_string();
    }

    func copy_from(source: &Array<T>) {
        for item in source {
            this.add(item);
        }
    }

    func raw_data() &T {
        return this.data;
    }

    func filter(predicate: func (value: T) bool) Array<T> {
        var result: Array<T> = [];
        for item in this {
            if predicate(item) {
                result.add(item);
            }
        }
        return result;
    }

    func map<U>(transform: func (value: T) U) Array<U> {
        var result: Array<U> = [];
        for item in this {
            result.add(transform(item));
        }
        return result;
    }
}

struct CString implements ops.CopyFrom<CString> {
    data: *char = null;

    mut func new(ptr: *char) {
        this.data = ptr;
    }

    mut func copy_from(source: &CString) {
        this.data = cx_cstring_copy(source.data);
    }

    mut func delete() {
        if this.data != null {
            delete this.data;
            this.data = null;
        }
    }

    func as_ptr() *char {
        return this.data;
    }
}

struct __CxString implements ops.Add {
    private data: *char = null;
    protected length: uint32 = 0;
    private is_static: uint32 = 0;

    static func format(fmt: string, ...values: any) string {
        var result: string = "";
        cx_string_format(&fmt, &values, &result);
        return result;
    }

    static func display(value: any) string {
        return string.format("{}", value);
    }

    func is_empty() bool {
        return this.length == 0;
    }

    func to_cstring() CString {
        return {cx_string_to_cstring(this as *string)};
    }

    func add(rhs: string) string {
        var result = string{};
        cx_string_concat(&result as *string, this as *string, &rhs as *string);
        return result;
    }

    func as_chars() Array<char> {
        var result: Array<char> = [];
        var i: uint32 = 0;
        while i < this.length {
            result.add(this.data[i]);
            i = i + 1;
        }
        return result;
    }
}

struct Map<K, V> implements ops.Index<K, V> {
    private data: *void;

    mut func new() {
        this.data = cx_map_new();
    }

    mut func delete() {
        cx_map_delete(this.data);
        this.data = null;
    }

    func remove(key: K) {
        var k: any = key;
        var h = HashBytes{};
        cx_hbytes(&k, &h);
        cx_map_remove(this.data, &h);
    }

    func find(key: K) ?&V {
        var k: any = key;
        var h = HashBytes{};
        cx_hbytes(&k, &h);
        var p = cx_map_find(this.data, &h) as *V;
        if p {
            return {p};
        }
        return null;
    }

    func index(key: K) &mut<V> {
        var k: any = key;
        var h = HashBytes{};
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
    private bytes: Array<char>;

    mut func new() {
        this.bytes = {};
    }

    func write(str: string) {
        cx_array_write_str(&this.bytes, &str);
    }

    func to_string() string {
        var str: string = "";
        cx_string_from_chars(this.bytes.raw_data(), this.bytes.length, &str);
        return str;
    }
}

interface Error {
    func message() string;
}

// Unit type for use as a void-like value type (e.g. Promise<Unit>)
struct Unit {}

// Promise for async operations
private struct PromiseState<T> {
    state: uint32 = 0; // 0=pending, 1=resolved, 2=rejected
    value: ?T = null;
    callbacks: Array<func (value: T)> = {};
}

struct Promise<T> implements ops.CopyFrom<Promise<T>> {
    protected data: Shared<PromiseState<T>>;

    func new() {
        this.data = {{}};
    }

    func delete() {
        this.data.delete();
    }

    func copy_from(source: &Promise<T>) {
        this.data.copy_from(&source.data);
    }

    func resolve(value: T) {
        var state = this.data.as_ref();
        if state.state != 0 {
            return;
        }
        state.state = 1;
        state.value = value;
        for var i = 0; i < state.callbacks.length; i = i + 1 {
            state.callbacks[i](value);
        }
    }

    func is_resolved() bool {
        return this.data.as_ref().state == 1;
    }

    func value() ?T {
        return this.data.as_ref().value;
    }

    func then(callback: func (value: T)) {
        var state = this.data.as_ref();
        if state.state == 1 {
            // Already resolved - invoke immediately
            callback(state.value!);
        } else {
            // Pending - add to callback list
            state.callbacks.add(callback);
        }
    }

    func ref_count() uint32 {
        return this.data.ref_count();
    }
}

func promise<T>(executor: func (resolve: func (value: T))) Promise<T> {
    var p = Promise<T>{};
    executor(func [p] (value) {
        p.resolve(value);
    });
    return p;
}

func sleep(ms: uint64) Promise<Unit> {
    return promise(func (resolve) {
        timeout(ms, func [resolve] () {
            resolve({});
        });
    });
}

