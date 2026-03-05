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
            panic(stringf("expected {}, got {}", kind_display(kind), kind_display(this.kind)));
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

    func to_float() float64 {
        this.assert_kind(JsonKind.Double);
        let result: float64 = 0.0;
        unsafe {
            cx_json_value_convert(this.data, JsonKind.Double.discriminator(), &result);
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

    func at(index: uint32) JsonValue {
        let new_value = JsonValue{};
        unsafe {
            cx_json_array_index(this.data, index, &new_value);
        }
        return new_value;
    }

    func is_null() bool {
        return this.kind == JsonKind.Null;
    }

    func is_string() bool {
        return this.kind == JsonKind.String;
    }

    func is_bool() bool {
        return this.kind == JsonKind.Bool;
    }

    func is_int() bool {
        return this.kind == JsonKind.Int64;
    }

    func is_float() bool {
        return this.kind == JsonKind.Double;
    }

    func is_array() bool {
        return this.kind == JsonKind.Array;
    }

    func is_object() bool {
        return this.kind == JsonKind.Object;
    }

    func has(key: string) bool {
        let val = this.get(key);
        return !val.is_null();
    }

    impl ops.CopyFrom<JsonValue> {
        mut func copy_from(source: &JsonValue) {
            unsafe {
                cx_json_value_copy(source.data, &this);
            }
        }
    }
}

func parse(str: string) JsonValue {
    let result = JsonValue{};
    unsafe {
        cx_parse_json(&str, &result);
    }
    return result;
}

