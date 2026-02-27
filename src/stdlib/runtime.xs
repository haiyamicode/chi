import "std/ops" as ops;
import "std/mem" as mem;

private struct HashBytes {
    data: *void = null;
    length: uint32 = 0;
}

extern "C" {
    private unsafe func cx_print(str: string);
    private unsafe func cx_printf(format: *string, values: *void);
    private unsafe func cx_array_new(dest: *void);
    private unsafe func cx_array_delete(dest: *void);
    private unsafe func cx_array_add(dest: *void, size: uint32) *void;
    private unsafe func cx_array_write_str(dest: *void, str: *string);
    private unsafe func cx_array_reserve(dest: *void, elem_size: uint32, new_cap: uint32);
    private unsafe func cx_print_any(value: *void);
    private unsafe func cx_print_number(value: uint64);
    private unsafe func cx_print_string(str: *string);
    private unsafe func cx_gc_alloc(size: uint32, destructor: *void) *void;
    private unsafe func cx_malloc(size: uint32, ignored: *void) *void;
    private unsafe func cx_free(address: *void);
    private unsafe func cx_memset(address: *void, v: uint8, n: uint32);
    private unsafe func __copy_from(dest: *void, src: *void, destruct_old: bool);
    private unsafe func cx_runtime_start(stack: *void);
    private unsafe func cx_set_program_vtable(ptr: *void);
    private unsafe func cx_runtime_stop();
    private unsafe func cx_panic(message: *string);
    private unsafe func cx_throw(
        type_info: *void,
        data_ptr: *void,
        vtable_ptr: *void,
        type_id: uint32
    );
    private unsafe func cx_get_error_type_info() *void;
    private unsafe func cx_get_error_data() *void;
    private unsafe func cx_get_error_vtable() *void;
    private unsafe func cx_get_error_type_id() uint32;
    private unsafe func cx_personality(...) int32;
    private unsafe func cx_timeout(delay: uint64, callback: *void);
    private unsafe func cx_string_format(format: *string, values: *void, str: *string);
    private unsafe func cx_string_from_chars(data: *void, size: uint32, str: *string);
    private unsafe func cx_string_delete(dest: *string);
    private unsafe func cx_string_copy(dest: *string, src: *string);
    private unsafe func cx_string_to_cstring(str: *string) *char;
    private unsafe func cx_string_concat(dest: *string, s1: *string, s2: *string);
    private unsafe func cx_cstring_copy(src: *char) *char;
    private unsafe func cx_hbytes(value: *any, result: *HashBytes);
    private unsafe func cx_map_new() *void;
    private unsafe func cx_map_delete(data: *void);
    private unsafe func cx_map_find(data: *void, key: *HashBytes) *void;
    private unsafe func cx_map_add(data: *void, key: *HashBytes, value: *void);
    private unsafe func cx_map_remove(data: *void, key: *HashBytes);
    private unsafe func cx_parse_json(str: *string, result: *void);
    private unsafe func cx_json_value_delete(data: *void);
    private unsafe func cx_json_value_get(data: *void, key: *string, result: *void);
    private unsafe func cx_json_value_convert(data: *void, kind: uint32, result: *void);
    private unsafe func cx_json_array_index(data: *void, index: uint32, result: *void);
    private unsafe func cx_json_array_length(data: *void) uint32;
    private unsafe func cx_json_value_copy(data: *void, result: *void);
    private unsafe func cx_file_read(path: *string, result: *string);
    private unsafe func cx_debug(ptr: *void);
    private unsafe func cx_capture_new(payload_size: uint32, captures_ti: *void, dtor: *void) *void;
    private unsafe func cx_capture_retain(capture_ptr: *void);
    private unsafe func cx_capture_release(capture_ptr: *void);
    private unsafe func cx_capture_get_type(capture_ptr: *void) *void;
    private unsafe func cx_capture_get_data(capture_ptr: *void) *void;
}

struct __CxEnumBase<T> {
    private __value: T = undefined;
    private __display_name: *string = undefined;

    impl ops.Display {
        func display() string {
            unsafe {
                var s = *this.__display_name;
                return stringf("{}", s);
            }
        }
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

struct Shared<T> {
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
                unsafe {
                    delete this.data;
                }
            }
        }
    }

    func delete() {
        this.release();
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

    impl ops.CopyFrom<Shared<T>> {
        func copy_from(source: &Shared<T>) {
            source.retain();
            this.data = source.data;
        }
    }

    impl ops.UnwrapMut<T> {
        func unwrap_mut() &mut T {
            return &mut this.data.value;
        }
    }

    impl ops.Display {
        func display() string {
            return stringf("{}", this.as_ref());
        }
    }
}

struct Box<T: ops.AllowUnsized> {
    private _ptr: *T = null;

    func new(ptr: &move T) {
        unsafe {
            this._ptr = ptr as *T;
        }
    }

    func from_ptr(ptr: *T) {
        unsafe {
            if this._ptr {
                delete this._ptr;
            }
        }
        this._ptr = ptr;
    }

    func delete() {
        unsafe {
            if this._ptr {
                delete this._ptr;
            }
        }
    }

    func as_ref() &T {
        return this.as_mut();
    }

    func as_mut() &mut T {
        unsafe {
            return this._ptr;
        }
    }

    impl ops.CopyFrom<Box<T>> {
        func copy_from(source: &Box<T>) {
            unsafe {
                this._ptr = mem.copy_from<T>(source._ptr as &T) as *T;
            }
        }
    }

    impl where T: ops.Sized {
        static func wrap(val: T) Box<T> {
            return {mem.copy_from<T>(&val)};
        }
    }

    impl ops.UnwrapMut<T> {
        mut func unwrap_mut() &mut T {
            unsafe {
                return this._ptr;
            }
        }
    }

    impl ops.Display {
        func display() string {
            unsafe {
                return stringf("{}", *this._ptr);
            }
        }
    }
}

// Internal lambda struct for compiler-generated closures.
// Captures are type-erased (CxCapture payload pointer) so lambdas can be converted across
// capture types with the same call signature.
struct __CxLambda {
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
            unsafe {
                cx_capture_release(this.captures);
            }
            this.captures = null;
        }
    }

    func as_ptr() *void {
        return this.fn_ptr;
    }

    func data_ptr() *void {
        unsafe {
            return cx_capture_get_data(this.captures);
        }
    }

    impl ops.CopyFrom<__CxLambda> {
        func copy_from(source: &__CxLambda) {
            // Retain the source captures
            if source.captures {
                unsafe {
                    cx_capture_retain(source.captures);
                }
            }

            // Copy all fields
            this.fn_ptr = source.fn_ptr;
            this.length = source.length;
            this.captures = source.captures;
        }
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

struct JsonValue {
    private data: *void = null;
    protected kind: JsonKind = JsonKind.Null;

    func delete() {
        unsafe {
            cx_json_value_delete(this.data);
        }
    }

    func get(key: string) JsonValue {
        let new_value = JsonValue{};
        unsafe {
            var key_ptr = &key;
            var value_ptr = &new_value;
            cx_json_value_get(this.data, key_ptr as *string, value_ptr as *void);
        }
        return new_value;
    }

    func assert_kind(kind: JsonKind) {
        if this.kind != kind {
            panic(
                stringf(
                    "expected {}, got {}",
                    json_kind_display(kind),
                    json_kind_display(this.kind)
                )
            );
        }
    }

    func to_string() string {
        this.assert_kind(JsonKind.String);
        let result = "";
        unsafe {
            cx_json_value_convert(this.data, JsonKind.String.discriminator(), &result);
        }
        return result;
    }

    func to_bool() bool {
        this.assert_kind(JsonKind.Bool);
        let result = false;
        unsafe {
            cx_json_value_convert(this.data, JsonKind.Bool.discriminator(), &result);
        }
        return result;
    }

    func to_int() int64 {
        this.assert_kind(JsonKind.Int64);
        let result: int64 = 0;
        unsafe {
            cx_json_value_convert(this.data, JsonKind.Int64.discriminator(), &result);
        }
        return result;
    }

    func length() uint32 {
        unsafe {
            return cx_json_array_length(this.data);
        }
    }

    func to_array() Array<JsonValue> {
        let result: Array<JsonValue> = [];
        let len = this.length();
        for i in 0..len {
            let new_value = JsonValue{};
            unsafe {
                cx_json_array_index(this.data, i, &new_value);
            }
            result.add(new_value);
        }
        return result;
    }

    impl ops.CopyFrom<JsonValue> {
        func copy_from(source: &JsonValue) {
            unsafe {
                cx_json_value_copy(source.data, &this);
            }
        }
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
    unsafe {
        cx_print_any(&value);
        cx_print("\n");
    }
}

func gc_alloc(size: uint32) *void {
    unsafe {
        return cx_gc_alloc(size, null);
    }
}

func printf(format: string, ...values: any) {
    unsafe {
        cx_printf(&format, &values);
    }
}

func panic(message: string) never {
    unsafe {
        cx_panic(&message);
    }
}

func timeout(delay: uint64, callback: func) {
    unsafe {
        cx_timeout(delay, &callback);
    }
}

func stringf(format: string, ...values: any) string {
    var str: string = "";
    unsafe {
        cx_string_format(&format, &values, &str);
    }
    return str;
}

func assert(cond: bool, message: ?string) {
    if !cond => if message {
        panic(stringf("assertion failed: {}", message));
    } else {
        panic("assertion failed");
    }
}

func json_parse(str: string) JsonValue {
    let result = JsonValue{};
    unsafe {
        cx_parse_json(&str, &result);
    }
    return result;
}

func fs_read(path: string) string {
    var result: string = "";
    unsafe {
        cx_file_read(&path, &result);
    }
    return result;
}

struct Array<T> {
    private data: *T = null;
    protected length: uint32 = 0;
    protected capacity: uint32 = 0;

    func new(...values: T) {
        unsafe {
            cx_array_new(&this);
        }
        if values.length > 0 {
            unsafe {
                cx_array_reserve(&this, sizeof T, values.length);
            }
            for value in values {
                this.add(value);
            }
        }
    }

    func delete() {
        unsafe {
            cx_free(this.data as *void);
        }
    }

    func add(item: T) {
        unsafe {
            var ptr = cx_array_add(&this, sizeof T) as *T;
            *ptr = item;
        }
    }

    func clear() {
        unsafe {
            if this.data {
                cx_free(this.data as *void);
            }
            cx_array_new(&this);
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

    func map<U>(transform: func (value: T, index: uint32) U) Array<U> {
        var result: Array<U> = [];
        for item, i in this {
            result.add(transform(item, i));
        }
        return result;
    }

    impl ops.IndexMut<uint32, T>, ops.IndexMutIterable<uint32, T> {
        func index_mut(index: uint32) &mut T {
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
    }

    impl ops.CopyFrom<Array<T>> {
        func copy_from(source: &Array<T>) {
            for item in source {
                this.add(item);
            }
        }
    }

    impl ops.Display {
        func display() string {
            var buf = Buffer{};
            buf.write("[");
            for item, i in this {
                if i > 0 {
                    buf.write(", ");
                }
                buf.write(stringf("{}", item));
            }
            buf.write("]");
            return buf.to_string();
        }
    }

    impl ops.Slice<Array<T>> {
        func slice(start: ?uint32, end: ?uint32) Array<T> {
            var s: uint32 = 0;
            var e: uint32 = this.length;
            if start {
                s = start;
            }
            if end {
                e = end;
            }
            assert(s <= e, "slice start must be <= end");
            assert(e <= this.length, "slice end out of bounds");
            var result: Array<T> = [];
            var i = s;
            while i < e {
                result.add(this.data[i]);
                i = i + 1;
            }
            return result;
        }
    }
}

struct CString {
    data: *char = null;

    mut func new(ptr: *char) {
        this.data = ptr;
    }

    mut func delete() {
        if this.data != null {
            unsafe {
                delete this.data;
            }
            this.data = null;
        }
    }

    func as_ptr() *char {
        return this.data;
    }

    impl ops.CopyFrom<CString> {
        mut func copy_from(source: &CString) {
            unsafe {
                this.data = cx_cstring_copy(source.data);
            }
        }
    }
}

struct __CxString {
    private data: *char = null;
    protected length: uint32 = 0;
    private is_static: uint32 = 0;

    static unsafe func from_char_ptr(data: *char, size: uint32) string {
        var result: string = "";
        cx_string_from_chars(data as *void, size, &result);
        return result;
    }

    func is_empty() bool {
        return this.length == 0;
    }

    func to_cstring() CString {
        unsafe {
            return {cx_string_to_cstring(&this as *string)};
        }
    }

    func to_chars() Array<char> {
        var result: Array<char> = [];
        var i: uint32 = 0;
        while i < this.length {
            result.add(this.data[i]);
            i = i + 1;
        }
        return result;
    }

    impl ops.Add {
        func add(rhs: string) string {
            var result = string{};
            unsafe {
                cx_string_concat(&result as *string, &this as *string, &rhs as *string);
            }
            return result;
        }
    }
}

struct Map<K, V> {
    private data: *void;

    mut func new() {
        unsafe {
            this.data = cx_map_new();
        }
    }

    mut func delete() {
        unsafe {
            cx_map_delete(this.data);
        }
        this.data = null;
    }

    func remove(key: K) {
        var k: any = key;
        var h = HashBytes{};
        unsafe {
            cx_hbytes(&k, &h);
            cx_map_remove(this.data, &h);
        }
    }

    func find(key: K) ?&V {
        var k: any = key;
        var h = HashBytes{};
        unsafe {
            cx_hbytes(&k, &h);
            var p = cx_map_find(this.data, &h) as *V;
            if p {
                return {p};
            }
        }
        return null;
    }

    impl ops.IndexMut<K, V> {
        func index_mut(key: K) &mut V {
            var k: any = key;
            var h = HashBytes{};
            unsafe {
                cx_hbytes(&k, &h);
                var p = cx_map_find(this.data, &h) as *V;
                if !p {
                    p = new V{};
                    cx_map_add(this.data, &h, p);
                }
                return p;
            }
        }
    }
}

struct Buffer {
    protected bytes: Array<char>;

    mut func new() {
        this.bytes = {};
    }

    func write(str: string) {
        unsafe {
            cx_array_write_str(&this.bytes, &str);
        }
    }

    func to_string() string {
        var str: string = "";
        unsafe {
            cx_string_from_chars(this.bytes.raw_data(), this.bytes.length, &str);
        }
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

struct Promise<T> {
    protected data: Shared<PromiseState<T>>;

    func new() {
        this.data = {{}};
    }

    static func make(executor: func (resolve: func (value: T))) Promise<T> {
        var p = Promise<T>{};
        executor(func [p] (value) {
            p.resolve(value);
        });
        return p;
    }

    func delete() {
        this.data.delete();
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

    impl ops.CopyFrom<Promise<T>> {
        func copy_from(source: &Promise<T>) {
            this.data.copy_from(&source.data);
        }
    }
}

func sleep(ms: uint64) Promise<Unit> {
    return Promise<Unit>.make(
        func (resolve) {
            timeout(ms, func [resolve] () {
                resolve({});
            });
        }
    );
}

