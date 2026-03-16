// std/json — JSON parsing and manipulation
import "std/ops" as ops;

extern "C" {
    unsafe func cx_parse_json(str: *string, result: *void);
    unsafe func cx_json_value_delete(data: *void);
    unsafe func cx_json_value_get(data: *void, key: *string, result: *void);
    unsafe func cx_json_value_convert(data: *void, kind: uint32, result: *void);
    unsafe func cx_json_array_index(data: *void, index: uint32, result: *void);
    unsafe func cx_json_array_length(data: *void) uint32;
    unsafe func cx_json_value_copy(data: *void, result: *void);
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
                    ValueKind.Null => "null",
                    ValueKind.Bool => "bool",
                    ValueKind.Int64 => "int64",
                    ValueKind.Uint64 => "uint64",
                    ValueKind.Double => "double",
                    ValueKind.String => "string",
                    ValueKind.Array => "array",
                    ValueKind.Object => "object",
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
            panic(stringf("expected {}, got {}", kind, this.kind));
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

export func parse(str: string) Value {
    let result = Value{};
    unsafe {
        cx_parse_json(&str, &result);
    }
    return result;
}
