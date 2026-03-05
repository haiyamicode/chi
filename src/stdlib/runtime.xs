import "std/ops" as ops;
import "std/mem" as mem;

extern "C" {
    unsafe func cx_print(str: string);
    unsafe func cx_printf(format: *string, values: *void);
    unsafe func cx_array_new(dest: *void);
    unsafe func cx_array_delete(dest: *void);
    unsafe func cx_array_add(dest: *void, size: uint32) *void;
    unsafe func cx_array_write_str(dest: *void, str: *string);
    unsafe func cx_array_reserve(dest: *void, elem_size: uint32, new_cap: uint32);
    unsafe func cx_array_append(dest: *void, src: *void, elem_size: uint32);
    unsafe func cx_print_any(value: *void);
    unsafe func cx_print_number(value: uint64);
    unsafe func cx_print_string(str: *string);
    unsafe func cx_gc_alloc(size: uint32, destructor: *void) *void;
    unsafe func cx_malloc(size: uint32, ignored: *void) *void;
    unsafe func cx_free(address: *void);
    unsafe func cx_memset(address: *void, v: uint8, n: uint32);
    unsafe func __copy_from(dest: *void, src: *void, destruct_old: bool);
    unsafe func cx_runtime_start(stack: *void);
    unsafe func cx_set_program_vtable(ptr: *void);
    unsafe func cx_runtime_stop();
    unsafe func cx_panic(message: *string);
    unsafe func cx_throw(type_info: *void, data_ptr: *void, vtable_ptr: *void, type_id: uint32);
    unsafe func cx_get_error_type_info() *void;
    unsafe func cx_get_error_data() *void;
    unsafe func cx_get_error_vtable() *void;
    unsafe func cx_get_error_type_id() uint32;
    unsafe func cx_personality(...) int32;
    unsafe func cx_timeout(delay: uint64, callback: *void);
    unsafe func cx_string_format(format: *string, values: *void, str: *string);
    unsafe func cx_string_from_chars(data: *void, size: uint32, str: *string);
    unsafe func cx_string_delete(dest: *string);
    unsafe func cx_string_copy(dest: *string, src: *string);
    unsafe func cx_string_to_cstring(str: *string) *byte;
    unsafe func cx_string_concat(dest: *string, s1: *string, s2: *string);
    unsafe func cx_cstring_copy(src: *byte) *byte;
    unsafe func cx_parse_json(str: *string, result: *void);
    unsafe func cx_json_value_delete(data: *void);
    unsafe func cx_json_value_get(data: *void, key: *string, result: *void);
    unsafe func cx_json_value_convert(data: *void, kind: uint32, result: *void);
    unsafe func cx_json_array_index(data: *void, index: uint32, result: *void);
    unsafe func cx_json_array_length(data: *void) uint32;
    unsafe func cx_json_value_copy(data: *void, result: *void);
    unsafe func cx_file_read(path: *string, result: *string);
    unsafe func cx_debug(ptr: *void);
    unsafe func cx_capture_new(payload_size: uint32, captures_ti: *void, dtor: *void) *void;
    unsafe func cx_capture_retain(capture_ptr: *void);
    unsafe func cx_capture_release(capture_ptr: *void);
    unsafe func cx_capture_get_type(capture_ptr: *void) *void;
    unsafe func cx_capture_get_data(capture_ptr: *void) *void;
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

struct SharedData<T> {
    ref_count: uint32;
    value: T;

    mut func new(v: T) {
        this.ref_count = 1;
        this.value = v;
    }
}

export struct Shared<T> {
    private data: *SharedData<T> = null;

    mut func new(value: T) {
        this.data = new SharedData<T>{value};
    }

    private mut func retain() {
        if this.data {
            this.data.ref_count = this.data.ref_count + 1;
        }
    }

    private mut func release() {
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

    mut func delete() {
        this.release();
    }

    func as_ref() &T {
        return &this.data.value;
    }

    mut func set(value: T) {
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
        mut func copy_from(source: &Shared<T>) {
            var ptr = source.data;
            if ptr {
                ptr.ref_count = ptr.ref_count + 1;
            }
            this.data = ptr;
        }
    }

    impl ops.UnwrapMut<T> {
        mut func unwrap_mut() &mut T {
            return &mut this.data.value;
        }
    }

    impl ops.Display {
        func display() string {
            return stringf("{}", this.as_ref());
        }
    }
}

export struct Box<T: ops.AllowUnsized> {
    private _ptr: *T = null;

    mut func new(ptr: &move T) {
        unsafe {
            this._ptr = (move ptr) as *T;
        }
    }

    mut func from_ptr(ptr: *T) {
        unsafe {
            if this._ptr {
                delete this._ptr;
            }
        }
        this._ptr = ptr;
    }

    mut func delete() {
        unsafe {
            if this._ptr {
                delete this._ptr;
            }
        }
    }

    func as_ref() &T {
        unsafe {
            return this._ptr;
        }
    }

    mut func as_mut() &mut T {
        unsafe {
            return this._ptr;
        }
    }

    impl ops.CopyFrom<Box<T>> {
        mut func copy_from(source: &Box<T>) {
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

    mut func new(fn: *void, len: uint32) {
        this.fn_ptr = fn;
        this.length = len;
    }

    mut func set_captures_ptr(ptr: *void) {
        this.captures = ptr;
    }

    mut func delete() {
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
        mut func copy_from(source: &__CxLambda) {
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

export enum JsonKind {
    Null,
    Bool,
    Int64,
    Uint64,
    Double,
    String,
    Array,
    Object
}

export struct JsonValue {
    private data: *void = null;
    protected kind: JsonKind = JsonKind.Null;

    mut func delete() {
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
            panic(stringf("expected {}, got {}", json_kind_display(kind), json_kind_display(this.kind)));
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
            result.push(new_value);
        }
        return result;
    }

    impl ops.CopyFrom<JsonValue> {
        mut func copy_from(source: &JsonValue) {
            unsafe {
                cx_json_value_copy(source.data, &this);
            }
        }
    }
}

export func json_kind_display(kind: JsonKind) string {
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

export func println(value: any) {
    unsafe {
        cx_print_any(&value);
        cx_print("\n");
    }
}

export func gc_alloc(size: uint32) *void {
    unsafe {
        return cx_gc_alloc(size, null);
    }
}

export func printf(format: string, ...values: any) {
    unsafe {
        cx_printf(&format, &values);
    }
}

export func panic(message: string) never {
    unsafe {
        cx_panic(&message);
    }
}

export func timeout(delay: uint64, callback: func) {
    unsafe {
        cx_timeout(delay, &callback);
    }
}

export func stringf(format: string, ...values: any) string {
    var str: string = "";
    unsafe {
        cx_string_format(&format, &values, &str);
    }
    return str;
}

export func assert(cond: bool, message: ?string) {
    if !cond => if message {
        panic(stringf("assertion failed: {}", message));
    } else {
        panic("assertion failed");
    }
}

export func json_parse(str: string) JsonValue {
    let result = JsonValue{};
    unsafe {
        cx_parse_json(&str, &result);
    }
    return result;
}

export func fs_read(path: string) string {
    var result: string = "";
    unsafe {
        cx_file_read(&path, &result);
    }
    return result;
}

export struct Array<T> {
    private data: *T = null;
    protected length: uint32 = 0;
    protected capacity: uint32 = 0;

    mut func new(...values: T) {
        unsafe {
            cx_array_new(&this);
        }
        if values.length > 0 {
            unsafe {
                cx_array_reserve(&this, sizeof T, values.length);
            }
            for value in values {
                this.push(value);
            }
        }
    }

    mut func delete() {
        unsafe {
            cx_free(this.data as *void);
        }
    }

    mut func push(item: T) {
        unsafe {
            var ptr = cx_array_add(&this, sizeof T) as *T;
            *ptr = item;
        }
    }

    mut func reserve(n: uint32) {
        unsafe {
            cx_array_reserve(&this, sizeof T, this.length + n);
        }
    }

    mut func clear() {
        unsafe {
            if this.data {
                cx_free(this.data as *void);
            }
            cx_array_new(&this);
        }
    }

    impl where T: ops.Int {
        // Resize to exactly n elements, zero-filling any new space.
        mut func resize_fill(n: uint32) {
            this.reserve(n);
            unsafe {
                cx_memset(this.data as *void, 0, n * sizeof T);
            }
            this.length = n;
        }

        // Shrink length to n without freeing memory.
        mut func truncate(n: uint32) {
            if n < this.length {
                this.length = n;
            }
        }
    }

    func raw_data() &T {
        return this.data;
    }

    func filter(predicate: func (value: T) bool) Array<T> {
        var result: Array<T> = [];
        for item in this {
            if predicate(item) {
                result.push(item);
            }
        }
        return result;
    }

    func map<U>(transform: func (value: T, index: uint32) U) Array<U> {
        var result: Array<U> = [];
        for item, i in this {
            result.push(transform(item, i));
        }
        return result;
    }

    impl ops.IndexMut<uint32, T>, ops.IndexMutIterable<uint32, T> {
        mut func index_mut(index: uint32) &mut T {
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
        mut func copy_from(source: &Array<T>) {
            for item in source {
                this.push(item);
            }
        }
    }

    impl ops.Display {
        func display() string {
            var buf = Buffer{};
            buf.write_string("[");
            for item, i in this {
                if i > 0 {
                    buf.write_string(", ");
                }
                buf.write_string(stringf("{}", item));
            }
            buf.write_string("]");
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
                result.push(this.data[i]);
                i = i + 1;
            }
            return result;
        }
    }

    private static func make_span(data: *T, length: uint32, start: ?uint32, end: ?uint32) []mut T {
        var s = start ?? 0;
        var e = end ?? length;
        assert(s <= e, "span start must be <= end");
        assert(e <= length, "span end out of bounds");
        unsafe {
            return {&data[s], e - s};
        }
    }

    func span(start: ?uint32, end: ?uint32) []T {
        return Array.make_span(this.data, this.length, start, end);
    }

    mut func span_mut(start: ?uint32, end: ?uint32) []mut T {
        return Array.make_span(this.data, this.length, start, end);
    }
}

export struct CString {
    data: *byte = null;

    mut func new(ptr: *byte) {
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

    func as_ptr() *byte {
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
    private data: *byte = null;
    protected length: uint32 = 0;
    private is_static: uint32 = 0;

    static unsafe func from_raw(data: *byte, size: uint32) string {
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

    func to_bytes() Array<byte> {
        var result: Array<byte> = [];
        var i: uint32 = 0;
        while i < this.length {
            result.push(this.data[i]);
            i = i + 1;
        }
        return result;
    }

    func span() []byte {
        unsafe {
            return {this.data, this.length};
        }
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

    impl ops.Eq {
        func eq(other: string) bool {
            if this.length != other.length {
                return false;
            }
            return mem.memcmp(this.data, other.data, this.length) == 0;
        }
    }

    impl ops.Ord {
        func cmp(other: string) int {
            var min_len = this.length;
            if other.length < min_len {
                min_len = other.length;
            }
            var result = mem.memcmp(this.data, other.data, min_len);
            if result != 0 {
                return result;
            }
            if this.length < other.length {
                return -1;
            }
            if this.length > other.length {
                return 1;
            }
            return 0;
        }
    }
}

struct __CxSpan<T> {
    private data: *T;
    protected length: uint32;

    mut unsafe func new(data: *T, length: uint32) {
        this.data = data;
        this.length = length;
    }

    func is_empty() bool {
        return this.length == 0;
    }

    impl ops.IndexMut<uint32, T>, ops.IndexMutIterable<uint32, T> {
        mut func index_mut(index: uint32) &mut T {
            assert(index < this.length, "index out of bounds");
            unsafe {
                return &mut this.data[index];
            }
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

    impl ops.Display {
        func display() string {
            var buf = Buffer{};
            buf.write_string("[");
            for item, i in this {
                if i > 0 {
                    buf.write_string(", ");
                }
                buf.write_string(stringf("{}", item));
            }
            buf.write_string("]");
            return buf.to_string();
        }
    }

    impl ops.Slice<[]T> {
        func slice(start: ?uint32, end: ?uint32) []T {
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
            unsafe {
                return {&this.data[s], e - s};
            }
        }
    }
}

interface Reader {
    func read(buf: []mut byte) uint32;
}

interface Writer {
    func write(data: []byte);
}

export struct Buffer {
    ...bytes: Array<byte>;
    read_pos: uint32 = 0;

    mut func new() {
        this.bytes = {};
    }

    static func alloc(n: uint32) Buffer {
        var buf = Buffer{};
        buf.bytes.resize_fill(n);
        return buf;
    }

    static func from_bytes(data: Array<byte>) Buffer {
        var buf = Buffer{};
        buf.write(data.span());
        return buf;
    }

    static func from_string(str: string) Buffer {
        var buf = Buffer{};
        buf.write_string(str);
        return buf;
    }

    mut func write_string(str: string) {
        unsafe {
            cx_array_write_str(&this.bytes, &str);
        }
    }

    func to_string() string {
        var str: string = "";
        unsafe {
            cx_string_from_chars(this.raw_data(), this.length, &str);
        }
        return str;
    }

    impl Writer {
        mut func write(data: []byte) {
            for b in data {
                this.bytes.push(b);
            }
        }
    }

    impl Reader {
        mut func read(buf: []mut byte) uint32 {
            if this.read_pos >= this.length {
                return 0;
            }
            var available = this.length - this.read_pos;
            var n = if buf.length < available => buf.length else => available;
            var i: uint32 = 0;
            while i < n {
                buf[i] = this.bytes[this.read_pos + i];
                i = i + 1;
            }
            this.read_pos = this.read_pos + n;
            return n;
        }
    }

    mut func truncate(n: uint32) {
        this.bytes.truncate(n);
    }
}

export interface Error {
    func message() string;
}

// Unit type for use as a void-like value type (e.g. Promise<Unit>)
export struct Unit {}

// Promise for async operations
struct PromiseState<T> {
    state: uint32 = 0; // 0=pending, 1=resolved, 2=rejected
    value: ?T = null;
    callbacks: Array<func (value: T)> = {};
}

export struct Promise<T> {
    protected data: Shared<PromiseState<T>>;

    mut func new() {
        this.data = {{}};
    }

    static func make(executor: func (resolve: func (value: T))) Promise<T> {
        var p = Promise<T>{};
        executor(func [p] (value) {
            p.resolve(value);
        });
        return p;
    }

    mut func resolve(value: T) {
        var state = this.data.unwrap_mut();
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

    mut func then(callback: func (value: T)) {
        var state = this.data.unwrap_mut();
        if state.state == 1 {
            // Already resolved - invoke immediately
            callback(state.value!);
        } else {
            // Pending - add to callback list
            state.callbacks.push(callback);
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

export func sleep(ms: uint64) Promise<Unit> {
    return Promise<Unit>.make(func (resolve) {
        timeout(ms, func [resolve] () {
            resolve({});
        });
    });
}

struct MapNode<K: ops.Hash + ops.Eq, V> {
    key: K;
    value: V;
    hash: uint64;
    next: *MapNode<K, V>;
}

export struct MapIterator<K: ops.Hash + ops.Eq, V> {
    private buckets: **MapNode<K, V>;
    private capacity: uint32;
    private bucket_idx: uint32;
    private current: *MapNode<K, V>;

    mut func new(buckets: **MapNode<K, V>, capacity: uint32) {
        this.buckets = buckets;
        this.capacity = capacity;
        this.bucket_idx = 0;
        this.current = null;
    }

    impl ops.MutIterator<V> {
        mut func next() ?(&mut V) {
            if this.current != null {
                unsafe {
                    var node = this.current;
                    this.current = node.next;
                    return {&mut node.value};
                }
            }
            while this.bucket_idx < this.capacity {
                unsafe {
                    var node = this.buckets[this.bucket_idx];
                    this.bucket_idx += 1;
                    if node != null {
                        this.current = node.next;
                        return {&mut node.value};
                    }
                }
            }
            return null;
        }
    }
}

export struct Map<K: ops.Hash + ops.Eq, V> {
    private buckets: **MapNode<K, V> = null;
    private capacity: uint32 = 0;
    private count: uint32 = 0;

    mut func new() {
        this.capacity = 16;
        unsafe {
            var size = 8 * (this.capacity as uint32);
            this.buckets = cx_malloc(size, null) as **MapNode<K, V>;
            cx_memset(this.buckets as *void, 0, size);
        }
    }

    mut func delete() {
        if this.buckets == null {
            return;
        }
        var i: uint32 = 0;
        while i < this.capacity {
            unsafe {
                var node = this.buckets[i];
                while node != null {
                    var next = node.next;
                    delete node;
                    node = next;
                }
            }
            i += 1;
        }
        unsafe {
            cx_free(this.buckets as *void);
        }
    }

    func get(key: K) ?&V {
        if this.buckets == null {
            return null;
        }
        var h = key.hash();
        var idx = (h % (this.capacity as uint64)) as uint32;
        unsafe {
            var node = this.buckets[idx];
            while node != null {
                if node.hash == h && node.key == key {
                    return {&node.value};
                }
                node = node.next;
            }
        }
        return null;
    }

    mut func set(key: K, value: V) {
        var h = key.hash();
        var idx = (h % (this.capacity as uint64)) as uint32;
        unsafe {
            var node = this.buckets[idx];
            while node != null {
                if node.hash == h && node.key == key {
                    node.value = value;
                    return;
                }
                node = node.next;
            }
            this.buckets[idx] = new MapNode<K, V>{
                key: key, value: value, hash: h, next: this.buckets[idx]};
        }
        this.count += 1;
        if this.count > this.capacity * 2 {
            this.resize(this.capacity * 10);
        }
    }

    private mut func resize(new_capacity: uint32) {
        unsafe {
            var new_size = 8 * new_capacity;
            var new_buckets = cx_malloc(new_size, null) as **MapNode<K, V>;
            cx_memset(new_buckets as *void, 0, new_size);
            var i: uint32 = 0;
            while i < this.capacity {
                var node = this.buckets[i];
                while node != null {
                    var next = node.next;
                    var new_idx = (node.hash % (new_capacity as uint64)) as uint32;
                    node.next = new_buckets[new_idx];
                    new_buckets[new_idx] = node;
                    node = next;
                }
                i += 1;
            }
            cx_free(this.buckets as *void);
            this.buckets = new_buckets;
            this.capacity = new_capacity;
        }
    }

    mut func remove(key: K) {
        if this.buckets == null {
            return;
        }
        var h = key.hash();
        var idx = (h % (this.capacity as uint64)) as uint32;
        unsafe {
            var prev: *MapNode<K, V> = null;
            var node = this.buckets[idx];
            while node != null {
                var next = node.next;
                if node.hash == h && node.key == key {
                    if prev != null {
                        prev.next = next;
                    } else {
                        this.buckets[idx] = next;
                    }
                    var to_delete = node;
                    node = null;
                    delete to_delete;
                    this.count -= 1;
                    return;
                }
                prev = node;
                node = next;
            }
        }
    }

    func keys() Array<K> {
        var result: Array<K> = [];
        var i: uint32 = 0;
        while i < this.capacity {
            unsafe {
                var node = this.buckets[i];
                while node != null {
                    result.push(node.key);
                    node = node.next;
                }
            }
            i += 1;
        }
        return result;
    }

    impl ops.Index<K, V> {
        func index(key: K) &V {
            if this.buckets == null {
                panic("map: key not found");
            }
            var h = key.hash();
            var idx = (h % (this.capacity as uint64)) as uint32;
            unsafe {
                var node = this.buckets[idx];
                while node != null {
                    if node.hash == h && node.key == key {
                        return &node.value;
                    }
                    node = node.next;
                }
            }
            panic("map: key not found");
        }
    }

    impl ops.MutIterable<V> {
        func to_iter_mut() MapIterator<K, V> {
            return {this.buckets, this.capacity};
        }
    }

    impl ops.CopyFrom<Map<K, V>> {
        mut func copy_from(source: &Map<K, V>) {
            this.new();
            var ks = source.keys();
            for k in ks {
                var v = source.get(k);
                if v {
                    this.set(k, *v);
                }
            }
        }
    }
}

