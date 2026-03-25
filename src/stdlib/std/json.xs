// std/json — JSON parsing and manipulation
import "std/ops" as ops;
import "std/reflect" as reflect;

struct RawParseError {
    detail: string = "";
    has_location: bool = false;
    offset: uint32 = 0;
    line: uint32 = 0;
    column: uint32 = 0;
}

extern "C" {
    unsafe func cx_parse_json(
        str: *string,
        allow_jsonc: bool,
        result: *void,
        error: *RawParseError
    ) bool;
    unsafe func cx_json_value_delete(data: *void);
    unsafe func cx_json_value_get(data: *void, key: *string, result: *void);
    unsafe func cx_json_value_convert(data: *void, kind: uint32, result: *void);
    unsafe func cx_json_array_index(data: *void, index: uint32, result: *void);
    unsafe func cx_json_array_length(data: *void) uint32;
    unsafe func cx_json_value_copy(data: *void, result: *void);
    unsafe func cx_array_new(dest: *void);
    unsafe func cx_array_add(dest: *void, elem_size: uint32) *void;
    unsafe func cx_memset(address: *void, v: uint8, n: uint32);
    unsafe func __move(dest: *void, src: *void, size: uint32);
}

struct RawArray {
    data: *void = null;
    length: uint32 = 0;
    capacity: uint32 = 0;
}

struct RawOptional {
    has_value: bool = false;
}

export struct ParseOptions {
    jsonc: bool = true;
}

export struct ParseLocation {
    offset: uint32 = 0;
    line: uint32 = 0;
    column: uint32 = 0;
}

export struct ParseError {
    detail: string = "";
    path: ?string = null;
    location: ?ParseLocation = null;

    impl Error {
        func message() string {
            var result = stringf("JSON parse error: {}", this.detail);
            if let path = this.path {
                result = stringf("{} at path '{}'", result, path);
            }
            if let location = this.location {
                result = stringf(
                    "{} (line {}, column {}, offset {})",
                    result,
                    location.line,
                    location.column,
                    location.offset
                );
            }
            return result;
        }
    }
}

export enum ValueKind {
    Null,
    Bool,
    Int64,
    Uint64,
    Double,
    String,
    Array,
    Object;

    struct {
        impl ops.Display {
            func display() string {
                return switch this {
                    Null => "null",
                    Bool => "bool",
                    Int64 => "int64",
                    Uint64 => "uint64",
                    Double => "double",
                    String => "string",
                    Array => "array",
                    Object => "object",
                    else => "unknown"
                };
            }
        }
    }
}

export struct Value {
    private data: *void = null;
    protected kind: ValueKind = ValueKind.Null;

    mut func delete() {
        unsafe {
            cx_json_value_delete(this.data);
        }
    }

    func get(key: string) Value {
        let new_value = Value{};
        unsafe {
            var key_ptr = &key;
            var value_ptr = &new_value;
            cx_json_value_get(this.data, key_ptr as *string, value_ptr as *void);
        }
        return new_value;
    }

    func assert_kind(kind: ValueKind) {
        if this.kind != kind {
            throw new ParseError{
                detail: stringf("expected {}, got {}", kind, this.kind),
                path: null,
                location: null
            };
        }
    }

    func to_string() string {
        this.assert_kind(ValueKind.String);
        let result = "";
        unsafe {
            cx_json_value_convert(this.data, ValueKind.String.discriminator(), &result);
        }
        return result;
    }

    func to_bool() bool {
        this.assert_kind(ValueKind.Bool);
        let result = false;
        unsafe {
            cx_json_value_convert(this.data, ValueKind.Bool.discriminator(), &result);
        }
        return result;
    }

    func to_int() int64 {
        this.assert_kind(ValueKind.Int64);
        let result: int64 = 0;
        unsafe {
            cx_json_value_convert(this.data, ValueKind.Int64.discriminator(), &result);
        }
        return result;
    }

    func to_uint() uint64 {
        this.assert_kind(ValueKind.Uint64);
        let result: uint64 = 0;
        unsafe {
            cx_json_value_convert(this.data, ValueKind.Uint64.discriminator(), &result);
        }
        return result;
    }

    func to_float() float64 {
        this.assert_kind(ValueKind.Double);
        let result: float64 = 0.0;
        unsafe {
            cx_json_value_convert(this.data, ValueKind.Double.discriminator(), &result);
        }
        return result;
    }

    func length() uint32 {
        unsafe {
            return cx_json_array_length(this.data);
        }
    }

    func to_array() Array<Value> {
        let result: Array<Value> = [];
        let len = this.length();
        for i in 0..len {
            let new_value = Value{};
            unsafe {
                cx_json_array_index(this.data, i, &new_value);
            }
            result.push(new_value);
        }
        return result;
    }

    func at(index: uint32) Value {
        let new_value = Value{};
        unsafe {
            cx_json_array_index(this.data, index, &new_value);
        }
        return new_value;
    }

    func is_null() bool {
        return this.kind == ValueKind.Null;
    }

    func is_string() bool {
        return this.kind == ValueKind.String;
    }

    func is_bool() bool {
        return this.kind == ValueKind.Bool;
    }

    func is_int() bool {
        return this.kind == ValueKind.Int64;
    }

    func is_float() bool {
        return this.kind == ValueKind.Double;
    }

    func is_uint() bool {
        return this.kind == ValueKind.Uint64;
    }

    func is_array() bool {
        return this.kind == ValueKind.Array;
    }

    func is_object() bool {
        return this.kind == ValueKind.Object;
    }

    func has(key: string) bool {
        let val = this.get(key);
        return !val.is_null();
    }

    impl ops.Copy {
        mut func copy(source: &This) {
            unsafe {
                cx_json_value_copy(source.data, &this);
            }
        }
    }
}

export func parse_raw(str: string, options: ?ParseOptions) Value {
    let result = Value{};
    let parse_options = options ?? {};
    var error = RawParseError{};
    var ok = false;
    unsafe {
        ok = cx_parse_json(&str, parse_options.jsonc, &result, &error);
    }
    if !ok {
        let location = if error.has_location => (ParseLocation{
            offset: error.offset,
            line: error.line,
            column: error.column
        } as ?ParseLocation) else => null;
        throw new ParseError{
            detail: move error.detail,
            path: null,
            :location
        };
    }
    return result;
}

func json_type_error(path: string, ty: reflect.Type, value: Value) never {
    throw new ParseError{
        detail: stringf("expected {}, got {}", ty.name(), value.kind),
        path: move path,
        location: null
    };
}

unsafe func json_write_value<T>(dest: *void, value: T) {
    var tmp = value;
    __move(dest, &tmp as *T as *void, sizeof T);
}

unsafe func json_write_64_from_words(dest: *void, words: *uint32) {
    var out = dest as *uint32;
    *out = *words;
    *(out + 1) = *(words + 1);
}

unsafe func json_write_int64(dest: *void, value: int64) {
    var tmp = value;
    json_write_64_from_words(dest, &tmp as *int64 as *uint32);
}

unsafe func json_write_uint64(dest: *void, value: uint64) {
    var tmp = value;
    json_write_64_from_words(dest, &tmp as *uint64 as *uint32);
}

unsafe func json_write_float64(dest: *void, value: float64) {
    var tmp = value;
    json_write_64_from_words(dest, &tmp as *float64 as *uint32);
}

unsafe func json_assign_int(path: string, value: Value, ty: reflect.Type, dest: *void) {
    let bits = ty.int_bits();
    if ty.int_is_unsigned() {
        var raw: uint64 = 0;
        if value.kind == ValueKind.Uint64 {
            raw = value.to_uint();
        } else if value.kind == ValueKind.Int64 {
            raw = value.to_int() as uint64;
        } else {
            json_type_error(path, ty, value);
        }

        if bits == 8 {
            json_write_value(dest, raw as uint8);
        } else if bits == 16 {
            json_write_value(dest, raw as uint16);
        } else if bits == 32 {
            json_write_value(dest, raw as uint32);
        } else if bits == 64 {
            json_write_uint64(dest, raw);
        } else {
            panic(stringf("unsupported unsigned integer width {}", bits));
        }
        return;
    }

    var raw: int64 = 0;
    if value.kind == ValueKind.Int64 {
        raw = value.to_int();
    } else if value.kind == ValueKind.Uint64 {
        raw = value.to_uint() as int64;
    } else {
        json_type_error(path, ty, value);
    }

    if bits == 8 {
        json_write_value(dest, raw as int8);
    } else if bits == 16 {
        json_write_value(dest, raw as int16);
    } else if bits == 32 {
        json_write_value(dest, raw as int32);
    } else if bits == 64 {
        json_write_int64(dest, raw);
    } else {
        panic(stringf("unsupported integer width {}", bits));
    }
}

unsafe func json_assign_float(path: string, value: Value, ty: reflect.Type, dest: *void) {
    var raw: float64 = 0.0;
    if value.kind == ValueKind.Double {
        raw = value.to_float();
    } else if value.kind == ValueKind.Int64 {
        raw = value.to_int() as float64;
    } else if value.kind == ValueKind.Uint64 {
        raw = value.to_uint() as float64;
    } else {
        json_type_error(path, ty, value);
    }

    if ty.float_bits() == 32 {
        json_write_value(dest, raw as float);
    } else if ty.float_bits() == 64 {
        json_write_float64(dest, raw);
    } else {
        panic(stringf("unsupported float width {}", ty.float_bits()));
    }
}

unsafe func json_assign_array(path: string, value: Value, ty: reflect.Type, dest: *void) {
    if !value.is_array() {
        json_type_error(path, ty, value);
    }

    let elem = ty.elem();
    var elem_type: reflect.Type = undefined;
    if elem {
        elem_type = elem;
    } else {
        panic(stringf("array type '{}' has no element type", ty.name()));
    }

    let existing = dest as *RawArray;
    if existing.data && ty.has_destructor() {
        ty.destroy(dest);
    }
    cx_array_new(dest);
    let len = value.length();
    var i: uint32 = 0;
    while i < len {
        let item = value.at(i);
        let slot = cx_array_add(dest, elem_type.size());
        let item_path = stringf("{}[{}]", path, i);
        json_assign(item_path, item, elem_type, slot);
        i = i + 1;
    }
}

unsafe func json_assign_optional(path: string, value: Value, ty: reflect.Type, dest: *void) {
    let elem = ty.elem();
    var elem_type: reflect.Type = undefined;
    if elem {
        elem_type = elem;
    } else {
        panic(stringf("optional type '{}' has no element type", ty.name()));
    }

    let optional = dest as *RawOptional;
    if value.is_null() {
        if optional.has_value && ty.has_destructor() {
            ty.destroy(dest);
        }
        optional.has_value = false;
        return;
    }

    let value_ptr = ((dest as *byte) + (ty.optional_value_offset() as int)) as *void;
    if !optional.has_value {
        cx_memset(value_ptr, 0, elem_type.size());
    }
    optional.has_value = true;
    json_assign(path, value, elem_type, value_ptr);
}

unsafe func json_assign(path: string, value: Value, ty: reflect.Type, dest: *void) {
    switch ty.kind() {
        Array => json_assign_array(path, value, ty, dest),
        Optional => json_assign_optional(path, value, ty, dest),
        Struct => {
            if !value.is_object() {
                json_type_error(path, ty, value);
            }
            for field in ty.fields() {
                let field_value = value.get(field.name());
                if field_value.is_null() {
                    continue;
                }
                var field_path = "";
                if path == "" {
                    field_path = field.name();
                } else {
                    field_path = path + "." + field.name();
                }
                json_assign(field_path, field_value, field.type(), field.ptr_at(dest));
            }
        },
        Bool => {
            if !value.is_bool() {
                json_type_error(path, ty, value);
            }
            json_write_value(dest, value.to_bool());
        },
        String => {
            if !value.is_string() {
                json_type_error(path, ty, value);
            }
            *(dest as *string) = value.to_string();
        },
        Int => json_assign_int(path, value, ty, dest),
        Byte => {
            if value.kind != ValueKind.Int64 && value.kind != ValueKind.Uint64 {
                json_type_error(path, ty, value);
            }
            var raw: uint64 = 0;
            if value.kind == ValueKind.Uint64 {
                raw = value.to_uint();
            } else {
                raw = value.to_int() as uint64;
            }
            json_write_value(dest, raw as byte);
        },
        Rune => {
            if value.kind != ValueKind.Int64 && value.kind != ValueKind.Uint64 {
                json_type_error(path, ty, value);
            }
            var raw: uint64 = 0;
            if value.kind == ValueKind.Uint64 {
                raw = value.to_uint();
            } else {
                raw = value.to_int() as uint64;
            }
            json_write_value(dest, raw as rune);
        },
        Float => json_assign_float(path, value, ty, dest),
        else => panic(stringf("unsupported type '{}'", ty.name()))
    }
}

export func parse_into<T>(str: string, dest: &T, options: ?ParseOptions) {
    let value = parse_raw(str, options);
    let dest_type = dest.(type);
    let elem = dest_type.elem();
    var struct_type: reflect.Type = undefined;
    if elem {
        struct_type = elem;
    } else {
        panic("expected a reference destination");
    }

    if struct_type.kind() != reflect.Kind.Struct {
        panic(stringf("expected a struct destination, got {}", struct_type.name()));
    }

    unsafe {
        json_assign("", value, struct_type, dest as *T as *void);
    }
}

export func parse<T: ops.Construct>(str: string, options: ?ParseOptions) T {
    var result = T{};
    parse_into(str, &result, options);
    return result;
}
