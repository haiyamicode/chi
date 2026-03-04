/*
 * Copyright (c) 2018 Hai Thanh Nguyen
 *
 * This file is part of chi, which is MIT licensed.
 * See http://opensource.org/licenses/MIT
 */

#include "sema.h"
#include "ast.h"
#include "context.h"
#include "errors.h"

using namespace cx;

bool ChiTypeFn::should_use_sret() { return !return_type->is_primitive_abi_type() && !is_extern; }

ChiTypeEnum *ChiTypeEnumValue::parent_enum() {
    assert(enum_type->kind == TypeKind::Enum);
    return &enum_type->data.enum_;
}

ChiStructMember *ChiTypeStruct::add_member(Context *allocator, const string &name, ast::Node *node,
                                           ChiType *resolved_type, bool is_layout_field) {
    auto member = allocator->create_struct_member();
    member->node = node;
    member->resolved_type = resolved_type;
    member->parent_struct = this;
    auto is_static = node->declspec().is_static();

    if (is_static) {
        static_members.add(member);
        static_member_table[name] = member;
        member->method_index = -1;
    } else {
        members.add(member);
        if (node->type == ast::NodeType::FnDef) {
            member->method_index = vtable_size++;
        } else if (is_layout_field) {
            member->field_index = fields.len;
            fields.add(member);
        }
        member_table[name] = member;
    }

    return member;
}


array<ChiStructMember *> ChiTypeStruct::own_fields() {
    array<ChiStructMember *> result;
    for (auto field : fields) {
        if (!field->is_promoted())
            result.add(field);
    }
    return result;
}

ChiStructMember *ChiTypeStruct::find_member(const string &name) {
    auto found = member_table.get(name);
    return found ? *found : nullptr;
}

ChiStructMember *ChiTypeStruct::find_static_member(const string &name) {
    auto found = static_member_table.get(name);
    return found ? *found : nullptr;
}

InterfaceImpl *ChiTypeStruct::add_interface(Context *allocator, ChiType *iface, ChiType *impl) {
    auto entry = allocator->create_interface_impl();
    entry->interface_type = iface;
    entry->impl_type = impl;
    interface_table[iface] = entry;
    interfaces.add(entry);
    return entry;
}

bool ChiTypeStruct::is_interface(ChiType *type) {
    return type->kind == TypeKind::Struct && is_interface(&type->data.struct_);
}

bool ChiTypeStruct::is_generic(ChiType *type) {
    return type->kind == TypeKind::Struct && type->data.struct_.type_params.len > 0;
}

bool ChiTypeStruct::is_pointer_type(ChiType *type) {
    return type->kind == TypeKind::Pointer || type->kind == TypeKind::Reference ||
           type->kind == TypeKind::MutRef || type->kind == TypeKind::MoveRef;
}

bool ChiTypeStruct::is_mutable_pointer(ChiType *type) {
    return type->kind == TypeKind::Pointer || type->kind == TypeKind::MutRef ||
           type->kind == TypeKind::MoveRef;
}

ChiStructMember *ChiTypeStruct::get_constructor() { return find_member("new"); }

ChiStructMember *ChiTypeStruct::get_constructor(ChiType *type) {
    if (type->kind == TypeKind::Struct) {
        return type->data.struct_.get_constructor();
    }
    return nullptr;
}

ChiStructMember *ChiTypeStruct::get_destructor(ChiType *type) {
    if (type->kind == TypeKind::Struct) {
        return type->data.struct_.find_member("delete");
    }
    return nullptr;
}

ChiEnumVariant *ChiTypeEnum::add_variant(Context *allocator, const string &name, ast::Node *node,
                                         ChiType *resolved_type) {
    auto member = allocator->create_enum_member();
    member->name = name;
    member->node = node;
    member->resolved_type = resolved_type;
    member->enum_ = this;
    variants.add(member);

    member->index = variants.len;
    variant_table[name] = member;

    assert(resolved_type->kind == TypeKind::EnumValue);
    resolved_type->data.enum_value.member = member;
    assert(node->type == ast::NodeType::EnumVariant);
    node->data.enum_variant.resolved_enum_variant = member;
    return member;
}

ChiEnumVariant *ChiTypeEnum::find_member(const string &name) {
    auto found = variant_table.get(name);
    return found ? *found : nullptr;
}

string ChiStructMember::get_name() { return node->name; }
Visibility ChiStructMember::get_visibility() {
    auto declspec = node->get_declspec();
    if (!declspec) {
        return Visibility::Public;
    }
    return declspec->get_visibility();
}

bool ChiStructMember::check_access(bool is_internal, bool is_write) {
    auto visibility = get_visibility();
    if (visibility == Visibility::Private) {
        return is_internal;
    }
    if (visibility == Visibility::Protected) {
        return is_internal || !is_write;
    }
    return true;
}

ast::Node *Scope::find_one(const string &symbol, bool recursive) {
    if (auto val = symbols.get(symbol)) {
        return *val;
    }
    if (parent && recursive) {
        return parent->find_one(symbol, recursive);
    }
    return nullptr;
}

ast::Node *Scope::find_export(const string &symbol) {
    auto node = find_one(symbol);
    if (!node || !node->declspec().is_exported()) {
        return nullptr;
    }
    return node;
}

array<ast::Node *> Scope::get_all() {
    array<ast::Node *> list = {};
    for (auto entry : symbols.data) {
        list.add(entry.second);
    }
    return list;
}

array<ast::Node *> Scope::get_all_recursive() {
    auto list = get_all();
    if (parent) {
        list.add_all(parent->get_all_recursive());
    }
    return list;
}

ast::Node *Scope::find_parent(ast::NodeType type) {
    for (auto scope = this; scope; scope = scope->parent) {
        if (!scope || !scope->owner) {
            break;
        }
        if (scope->owner->type == type) {
            return scope->owner;
        }
    }
    return nullptr;
}

void Scope::put(const string &name, ast::Node *node) { symbols[name] = node; }

ChiType *ChiTypeFn::get_param_at(size_t index) {
    if (index < get_va_start()) {
        return params[index];
    }
    // For extern C variadic functions, there's no variadic parameter in params
    // Return nullptr to indicate any type is allowed for variadic args
    if (is_extern && is_variadic) {
        return nullptr;
    }
    // For Chi variadic functions, the last parameter is Array<T>, get its element type
    return params.last()->get_elem();
}

int ChiTypeFn::get_va_start() { return params.len - (int)(is_variadic && !is_extern); }

ChiType *ChiType::get_elem() {
    switch (kind) {
    case TypeKind::Pointer:
    case TypeKind::Optional:
    case TypeKind::Reference:
    case TypeKind::MutRef:
    case TypeKind::MoveRef:
    case TypeKind::Array:
        return data.array.elem;
    case TypeKind::FixedArray:
        return data.fixed_array.elem;
    case TypeKind::Span:
        return data.span.elem;
    case TypeKind::Result:
        return data.result.value;
    case TypeKind::Promise:
        return data.promise.value;
    case TypeKind::This:
        return data.pointer.elem;
    default:
        unreachable();
        return nullptr;
    }
}
