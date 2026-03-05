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
    unsafe func cx_string_format(format: *string, values: *void, str: *string);
    unsafe func cx_string_from_chars(data: *void, size: uint32, str: *string);
    unsafe func cx_string_delete(dest: *string);
    unsafe func cx_string_copy(dest: *string, src: *string);
    unsafe func cx_string_to_cstring(str: *string) *byte;
    unsafe func cx_string_concat(dest: *string, s1: *string, s2: *string);
    unsafe func cx_cstring_copy(src: *byte) *byte;
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

    mut func as_mut() &mut T {
        return &mut this.data.value;
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

    impl ops.Deref<T> {
        func deref() &T {
            return &this.data.value;
        }
    }

    impl ops.DerefMut<T> {
        mut func deref_mut() &mut T {
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

    impl ops.Deref<T> {
        func deref() &T {
            unsafe {
                return this._ptr;
            }
        }
    }

    impl ops.DerefMut<T> {
        mut func deref_mut() &mut T {
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

    func byte_span() []byte {
        unsafe {
            return {this.data, this.length};
        }
    }

    func byte_length() uint32 {
        return this.length;
    }

    func byte_at(i: uint32) byte {
        assert(i < this.length, "byte_at: index out of bounds");
        unsafe {
            return this.data[i];
        }
    }

    func byte_slice(start: uint32, end: uint32) string {
        assert(start <= end, "byte_slice: start must be <= end");
        assert(end <= this.length, "byte_slice: end out of bounds");
        unsafe {
            return string.from_raw(&this.data[start], end - start);
        }
    }

    // byte length of the UTF-8 codepoint starting at byte offset i
    private func cp_len(i: uint32) uint32 {
        unsafe {
            let b = this.data[i];
            if b < 0x80 {
                return 1;
            }
            if b < 0xE0 {
                return 2;
            }
            if b < 0xF0 {
                return 3;
            }
            return 4;
        }
    }

    // byte offset of the n-th codepoint (O(n))
    private func cp_offset(n: uint32) uint32 {
        var pos: uint32 = 0;
        var count: uint32 = 0;
        while count < n && pos < this.length {
            pos += this.cp_len(pos);
            count += 1;
        }
        return pos;
    }

    func char_length() uint32 {
        var pos: uint32 = 0;
        var count: uint32 = 0;
        while pos < this.length {
            pos += this.cp_len(pos);
            count += 1;
        }
        return count;
    }

    func at(i: uint32) rune {
        var pos = this.cp_offset(i);
        assert(pos < this.length, "at: index out of bounds");
        unsafe {
            let b0 = this.data[pos] as uint32;
            let n = this.cp_len(pos);
            if n == 1 {
                return b0 as rune;
            }
            if n == 2 {
                let b1 = this.data[pos + 1] as uint32;
                return ((b0 & 0x1F) << 6 | (b1 & 0x3F)) as rune;
            }
            if n == 3 {
                let b1 = this.data[pos + 1] as uint32;
                let b2 = this.data[pos + 2] as uint32;
                return ((b0 & 0xF) << 12 | (b1 & 0x3F) << 6 | (b2 & 0x3F)) as rune;
            }
            let b1 = this.data[pos + 1] as uint32;
            let b2 = this.data[pos + 2] as uint32;
            let b3 = this.data[pos + 3] as uint32;
            return ((b0 & 0x7) << 18 | (b1 & 0x3F) << 12 | (b2 & 0x3F) << 6 | (b3 & 0x3F)) as rune;
        }
    }

    impl ops.Slice<string> {
        func slice(start: ?uint32, end: ?uint32) string {
            var s: uint32 = 0;
            if start {
                s = start;
            }
            var byte_start = this.cp_offset(s);
            var byte_end = this.length;
            if end {
                byte_end = this.cp_offset(end);
            }
            return this.byte_slice(byte_start, byte_end);
        }
    }

    func contains(substr: string) bool {
        if substr.length == 0 {
            return true;
        }
        if substr.length > this.length {
            return false;
        }
        var i: uint32 = 0;
        let limit = this.length - substr.length;
        while i <= limit {
            if mem.memcmp(&this.data[i], substr.data, substr.length) == 0 {
                return true;
            }
            i += 1;
        }
        return false;
    }

    func starts_with(prefix: string) bool {
        if prefix.length > this.length {
            return false;
        }
        return mem.memcmp(this.data, prefix.data, prefix.length) == 0;
    }

    func ends_with(suffix: string) bool {
        if suffix.length > this.length {
            return false;
        }
        unsafe {
            return mem.memcmp(&this.data[this.length - suffix.length], suffix.data, suffix.length) == 0;
        }
    }

    func repeat(n: uint32) string {
        if n == 0 {
            return "";
        }
        var buf = Buffer{};
        var i: uint32 = 0;
        while i < n {
            buf.write_string(this);
            i += 1;
        }
        return buf.to_string();
    }

    func split(sep: string) Array<string> {
        var result: Array<string> = [];
        if sep.length == 0 {
            result.push(this);
            return result;
        }
        var start: uint32 = 0;
        var i: uint32 = 0;
        let limit = this.length - sep.length;
        while i <= limit {
            if mem.memcmp(&this.data[i], sep.data, sep.length) == 0 {
                result.push(this.byte_slice(start, i));
                i += sep.length;
                start = i;
            } else {
                i += 1;
            }
        }
        result.push(this.byte_slice(start, this.length));
        return result;
    }

    func replace_all(old: string, new_val: string) string {
        if old.length == 0 {
            return this;
        }
        var buf = Buffer{};
        var start: uint32 = 0;
        var i: uint32 = 0;
        let limit = this.length - old.length;
        while i <= limit {
            if mem.memcmp(&this.data[i], old.data, old.length) == 0 {
                buf.write_string(this.byte_slice(start, i));
                buf.write_string(new_val);
                i += old.length;
                start = i;
            } else {
                i += 1;
            }
        }
        buf.write_string(this.byte_slice(start, this.length));
        return buf.to_string();
    }

    func trim() string {
        var start: uint32 = 0;
        var end = this.length;
        while start < end && this.data[start] <= ' ' {
            start += 1;
        }
        while end > start && this.data[end - 1] <= ' ' {
            end -= 1;
        }
        return this.byte_slice(start, end);
    }

    func trim_left() string {
        var start: uint32 = 0;
        while start < this.length && this.data[start] <= ' ' {
            start += 1;
        }
        return this.byte_slice(start, this.length);
    }

    func trim_right() string {
        var end = this.length;
        while end > 0 && this.data[end - 1] <= ' ' {
            end -= 1;
        }
        return this.byte_slice(0, end);
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

    func as_ptr() *T {
        return this.data;
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

interface Read {
    func read(buf: []mut byte) uint32;
}

interface Write {
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

    impl Write {
        mut func write(data: []byte) {
            for b in data {
                this.bytes.push(b);
            }
        }
    }

    impl Read {
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
        if this.data.state != 0 {
            return;
        }
        this.data.state = 1;
        this.data.value = value;
        for var i = 0; i < this.data.callbacks.length; i = i + 1 {
            this.data.callbacks[i](value);
        }
    }

    func is_resolved() bool {
        return this.data.state == 1;
    }

    func value() ?T {
        return this.data.value;
    }

    mut func then(callback: func (value: T)) {
        if this.data.state == 1 {
            // Already resolved - invoke immediately
            callback(this.data.value!);
        } else {
            // Pending - add to callback list
            this.data.callbacks.push(callback);
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
            this.buckets[idx] = new MapNode<K, V>{key: key, value: value, hash: h, next: this.buckets[idx]};
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

