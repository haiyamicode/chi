import "std/ops" as ops;

export enum Kind {
    TypeSymbol,
    Fn,
    Void,
    Int,
    Float,
    Bool,
    Byte,
    Rune,
    String,
    Struct,
    Pointer,
    Reference,
    MutRef,
    MutexRef,
    MoveRef,
    Array,
    FixedArray,
    Span,
    Enum,
    EnumValue,
    Any,
    Subtype,
    Placeholder,
    Optional,
    FnLambda,
    Promise,
    Infer,
    Module,
    This_,
    ThisType_,
    Unknown,
    Bytes,
    Undefined,
    ZeroInit,
    Never,
    Unit,
    Tuple,
    Null
}

extern "C" {
    unsafe func cx_string_from_chars(data: *void, size: uint32, str: *string);
    unsafe func __reflect_dyn_elem(parent: *void, value: *void) *void;
    unsafe func cx_typeinfo_destroy(type_info: *void, data: *void);
}

struct __TypeFieldEntry {
    type_info: *void = null;
    offset: int32 = 0;
    visibility: int32 = 0;
    name_len: uint32 = 0;
    name_bytes: [256]byte = zeroinit;
}

struct __TypeIntData {
    bit_count: int32 = 0;
    is_unsigned: bool = false;
}

struct __TypeFloatData {
    bit_count: int32 = 0;
}

struct __TypePointerInfoData {
    elem: *void = null;
    elem_offset: int32 = 0;
}

struct __TypeArrayInfoData {
    internal: *void = null;
    elem: *void = null;
}

struct __TypeInfo {
    kind_value: int32 = 0;
    size_value: int32 = 0;
    data: [2]uint64 = zeroinit;
    destructor: *void = null;
    copier: *void = null;
    meta_table_len: int32 = 0;
    meta_table: *void = null;
    field_table_len: int32 = 0;
    field_table: *__TypeFieldEntry = null;
    name_len: uint32 = 0;
    name_bytes: [256]byte = zeroinit;
}

unsafe func string_from_inline_name(bytes: *byte, len: uint32) string {
    let result = "";
    cx_string_from_chars(bytes as *void, len, &result);
    return result;
}

unsafe func type_name_from_info(type_info: *__TypeInfo) string {
    if !type_info {
        return "";
    }
    return string_from_inline_name(&type_info.name_bytes[0], type_info.name_len);
}

unsafe func type_int_data(type_info: *__TypeInfo) *__TypeIntData {
    return &type_info.data[0] as *uint64 as *byte as *__TypeIntData;
}

unsafe func type_float_data(type_info: *__TypeInfo) *__TypeFloatData {
    return &type_info.data[0] as *uint64 as *byte as *__TypeFloatData;
}

unsafe func type_pointer_data(type_info: *__TypeInfo) *__TypePointerInfoData {
    return &type_info.data[0] as *uint64 as *byte as *__TypePointerInfoData;
}

unsafe func type_array_data(type_info: *__TypeInfo) *__TypeArrayInfoData {
    return &type_info.data[0] as *uint64 as *byte as *__TypeArrayInfoData;
}

func kind_name(kind: Kind) string {
    return switch kind {
        Kind.TypeSymbol => "type",
        Kind.Fn => "fn",
        Kind.Void => "void",
        Kind.Int => "int",
        Kind.Float => "float",
        Kind.Bool => "bool",
        Kind.Byte => "byte",
        Kind.Rune => "rune",
        Kind.String => "string",
        Kind.Struct => "struct",
        Kind.Pointer => "pointer",
        Kind.Reference => "ref",
        Kind.MutRef => "mutref",
        Kind.MutexRef => "mutexref",
        Kind.MoveRef => "moveref",
        Kind.Array => "array",
        Kind.FixedArray => "fixed_array",
        Kind.Span => "span",
        Kind.Enum => "enum",
        Kind.EnumValue => "enum_value",
        Kind.Any => "any",
        Kind.Subtype => "subtype",
        Kind.Placeholder => "placeholder",
        Kind.Optional => "optional",
        Kind.FnLambda => "lambda",
        Kind.Promise => "promise",
        Kind.Infer => "infer",
        Kind.Module => "module",
        Kind.This_ => "this",
        Kind.ThisType_ => "this_type",
        Kind.Unknown => "unknown",
        Kind.Bytes => "bytes",
        Kind.Undefined => "undefined",
        Kind.ZeroInit => "zeroinit",
        Kind.Never => "never",
        Kind.Unit => "unit",
        Kind.Tuple => "tuple",
        Kind.Null => "null",
        else => "unknown"
    };
}

export struct Field {
    private raw: *__TypeFieldEntry = null;

    static func from_raw(raw: *void) Field {
        var field = Field{};
        unsafe {
            field.raw = raw as *__TypeFieldEntry;
        }
        return field;
    }

    func is_valid() bool {
        return this.raw != null;
    }

    func name() string {
        if !this.raw {
            return "";
        }
        unsafe {
            return string_from_inline_name(&this.raw.name_bytes[0], this.raw.name_len);
        }
    }

    func type_name() string {
        if !this.raw {
            return "";
        }
        unsafe {
            return type_name_from_info(this.raw.type_info as *__TypeInfo);
        }
    }

    func type() Type {
        return {this.raw.type_info};
    }

    func offset() uint32 {
        return this.raw.offset as uint32;
    }

    unsafe func ptr_at(base: *void) *void {
        return ((base as *byte) + (this.raw.offset as int)) as *void;
    }

    impl ops.Display {
        func display() string {
            if !this.raw {
                return "<field>";
            }
            return stringf("{}: {}", this.name(), this.type_name());
        }
    }
}

export struct Type {
    private raw: *__TypeInfo = undefined;

    mut func new(raw: *void) {
        unsafe {
            this.raw = raw as *__TypeInfo;
        }
    }

    func kind() Kind {
        return this.raw.kind_value as Kind;
    }

    func name() string {
        unsafe {
            return type_name_from_info(this.raw);
        }
    }

    func qualified_name() string {
        return this.name();
    }

    func size() uint32 {
        return this.raw.size_value as uint32;
    }

    func elem() ?Type {
        let kind = this.kind();
        if kind == Kind.Pointer || kind == Kind.Reference || kind == Kind.MutRef || kind == Kind.MutexRef || kind == Kind.MoveRef || kind == Kind.Optional {
            unsafe {
                let ptr_data = type_pointer_data(this.raw);
                if !ptr_data.elem {
                    return null;
                }
                return Type{ptr_data.elem};
            }
        }
        if kind == Kind.Array {
            unsafe {
                let array_data = type_array_data(this.raw);
                if !array_data.elem {
                    return null;
                }
                return Type{array_data.elem};
            }
        }
        return null;
    }

    func int_bits() uint32 {
        unsafe {
            return type_int_data(this.raw).bit_count as uint32;
        }
    }

    func int_is_unsigned() bool {
        unsafe {
            return type_int_data(this.raw).is_unsigned;
        }
    }

    func float_bits() uint32 {
        unsafe {
            return type_float_data(this.raw).bit_count as uint32;
        }
    }

    func has_destructor() bool {
        return this.raw.destructor != null;
    }

    unsafe func destroy(ptr: *void) {
        cx_typeinfo_destroy(this.raw as *void, ptr);
    }

    func optional_value_offset() uint32 {
        unsafe {
            return type_pointer_data(this.raw).elem_offset as uint32;
        }
    }

    func dyn_elem<T: ops.Unsized>(value: &T) ?Type {
        unsafe {
            let raw = __reflect_dyn_elem(this.raw, &value as *void);
            if !raw {
                return null;
            }
            return Type{raw};
        }
    }

    func field_count() uint32 {
        if !this.raw.field_table {
            return 0;
        }
        return this.raw.field_table_len as uint32;
    }

    func field(index: uint32) ?Field {
        if !this.raw.field_table || index >= this.field_count() {
            return null;
        }
        unsafe {
            return Field.from_raw(&this.raw.field_table[index]);
        }
    }

    func fields() Array<Field> {
        let result: Array<Field> = [];
        let count = this.field_count();
        var i: uint32 = 0;
        while i < count {
            let field = this.field(i);
            if field {
                result.push(field);
            }
            i = i + 1;
        }
        return result;
    }

    impl ops.Display {
        func display() string {
            let name = this.name();
            let kind = kind_name(this.kind());
            let size = this.size();
            let field_count = this.field_count();
            if field_count > 0 {
                return stringf("{} ({}, {}b, {} fields)", name, kind, size, field_count);
            }

            let elem = this.elem();
            if elem {
                return stringf("{} ({}, {}b, elem={})", name, kind, size, elem.name());
            }

            return stringf("{} ({}, {}b)", name, kind, size);
        }
    }
}
