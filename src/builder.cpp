/*
 * Copyright (c) 2018 Hai Thanh Nguyen
 *
 * This file is part of chi, which is MIT licensed.
 * See http://opensource.org/licenses/MIT
 */

#include <jit/jit-dump.h>
#include <Zydis/DecoderTypes.h>

#include "builder.h"
#include "parser.h"
#include "ast_printer.h"
#include "jit.h"

using namespace cx;

#define ASM_NINT_DT ".quad"
#define ASM_CALL "callq"
#define ASM_ADDR_SUFFIX "(%rip)"

BuildContext::BuildContext(cx::Allocator* allocator) :
        resolve_ctx(new ResolveContext(allocator)),
        jit_ctx(new jit::CompileContext(resolve_ctx.get())) {
}

jit::Compiler BuildContext::create_compiler() {
    return {jit_ctx.get()};
}

Builder::Builder() : m_ctx(this) {
    auto resolver = m_ctx.create_resolver();
    resolver.context_init_builtins();
    auto jitc = m_ctx.create_compiler();
    for (auto node: resolver.get_context()->builtins) {
        if (node->type == NodeType::FnDef) {
            jitc.add_fn_node(node);
        }
    }
}

void Builder::process_file(ast::Package* package, const string& file_name) {
    auto src = io::Buffer::from_file(file_name);
    auto parts = string_split(file_name, ".");
    auto kind = ModuleKind::CX;
    if (!parts.empty()) {
        auto ext = parts[parts.size() - 1];
        if (ext == "h") {
            kind = ModuleKind::HEADER;
        }
    }

    auto module = package->modules.emplace();
    module->package = package;
    module->path = file_name;
    module->kind = kind;

    Tokenization tokenization;
    Lexer lexer(&src, &tokenization);
    lexer.tokenize();
    if (tokenization.error) {
        print("{}:{}:{}: error: {}\n", module->path, tokenization.error_pos.line + 1,
              tokenization.error_pos.col + 1, *tokenization.error);
        exit(0);
    }

    auto resolver = m_ctx.create_resolver();
    ScopeResolver scope_resolver(&resolver);
    ParseContext pc;
    pc.resolver = &scope_resolver;
    pc.module = module;
    pc.tokens = &tokenization.tokens;
    pc.allocator = this;

    Parser parser(&pc);
    parser.parse();
//    print_ast(pc.module->root);
    resolver.resolve(package);

    auto jitc = m_ctx.create_compiler();
    jitc.compile_internals();
    jitc.compile_module(module);
    auto entry_fn = jitc.get_context()->function_table[package->entry_fn];

    switch (m_build_mode) {
        case BuildMode::Run: {
            auto& main_fn = entry_fn;
            main_fn->apply(NULL, NULL);
            break;
        }
        case BuildMode::Executable: {
            AotCompilation ctx;
            ctx.compiler = &jitc;
            ctx.entry_fn = entry_fn;
            build_binary(&ctx);
            break;
        }
    }
}


void Builder::build_program(const string& entry_file_name) {
    process_file(add_package(), entry_file_name);
}

Node* Builder::create_node(NodeType type) {
    return m_ast_nodes.emplace(new Node(type))->get();
}

ChiType* Builder::create_type(TypeKind kind) {
    return m_types.emplace(new ChiType(kind, m_types.size + 1))->get();
}

void Builder::set_build_mode(BuildMode value) {
    m_build_mode = value;
    if (value != BuildMode::Run) {
        m_ctx.jit_ctx->enable_aot_compilation(true);
    }
}

bool Builder::generate_insn_asm(AotCompilation* ctx, AotFunctionInput* input, AssemblyState* as, FILE* stream) {
    auto& insn = as->instruction;
    auto jctx = ctx->compiler->get_context();
    if (insn.mnemonic == ZYDIS_MNEMONIC_MOV) {
        auto& dest = insn.operands[0];
        auto& src = insn.operands[1];
        if (dest.type == ZYDIS_OPERAND_TYPE_REGISTER && dest.reg.value == ZYDIS_REGISTER_R11) {
            auto fid = src.imm.value.s;
            as->fn_call = ctx->symbol_names.get(fid);
            if (as->fn_call) {
                return true;
            }
        } else if (src.type == ZYDIS_OPERAND_TYPE_IMMEDIATE) {
            auto value = src.imm.value.s;
            char buffer[256];
            ZydisFormatterFormatOperand(&ctx->formatter, &insn, 0, buffer, sizeof(buffer), 0);
            if (auto name = ctx->symbol_names.get(value)) {
                fmt::print(stream, "\tleaq {}{}, {}\n", *name, ASM_ADDR_SUFFIX, buffer);
                return true;
            }
        }

    } else if (insn.mnemonic == ZYDIS_MNEMONIC_MOVSXD || insn.mnemonic == ZYDIS_MNEMONIC_MOVSX) {
        char buffer[256];
        ZydisFormatterFormatInstruction(&ctx->formatter, &insn, buffer, sizeof(buffer), 0);
        fmt::print(stream, "\t#{}\n", buffer);
        for (int i = 0; i < insn.length; i++) {
            fmt::print(stream, "\t.byte {}\n", input->instructions->at(as->offset + i));
        }
        return true;

    } else if (insn.meta.category == ZYDIS_CATEGORY_COND_BR || insn.mnemonic == ZYDIS_MNEMONIC_JMP) {
        auto rel = insn.operands[0].imm.value.s;
        auto abs = as->offset + insn.length + rel;
        string label = as->labels[abs];
        fmt::print(stream, "\t{} {}\n", ZydisMnemonicGetString(insn.mnemonic), label);
        return true;

    } else if (insn.mnemonic == ZYDIS_MNEMONIC_CALL) {
        auto& op1 = insn.operands[0];
        if (as->fn_call) {
            fmt::print(stream, "\t{} {}\n", ASM_CALL, *as->fn_call);
            return true;
        } else if (op1.type == ZYDIS_OPERAND_TYPE_IMMEDIATE && op1.imm.value.s < 0) {
            auto fid = -op1.imm.value.s;
            fmt::print(stream, "\t{} {}\n", ASM_CALL, jctx->fn_symbols[fid - 1]);
            return true;
        } else if (op1.type == ZYDIS_OPERAND_TYPE_REGISTER) {
            char buffer[256];
            ZydisFormatterFormatOperand(&ctx->formatter, &insn, 0, buffer, sizeof(buffer), 0);
            fmt::print(stream, "\t{} *{}\n", ASM_CALL, buffer);
            return true;
        }
    }
    return false;
}

void Builder::generate_fn_asm(AotCompilation* ctx, AotFunctionInput* fn, FILE* stream) {
    auto decoder = &ctx->decoder;
    auto formatter = &ctx->formatter;
    auto data = fn->instructions->items;
    const ZyanUSize length = fn->instructions->size;
    AssemblyState as;
    auto& insn = as.instruction;

    // scan for labels
    as.offset = 0;
    while (ZYAN_SUCCESS(ZydisDecoderDecodeBuffer(decoder, data + as.offset, length - as.offset,
                                                 &insn))) {
        if (insn.meta.category == ZYDIS_CATEGORY_COND_BR || insn.mnemonic == ZYDIS_MNEMONIC_JMP) {
            auto rel = insn.operands[0].imm.value.s;
            auto abs = as.offset + insn.length + rel;
            if (!as.labels.get(abs)) {
                auto id = int(as.labels.data.size() + 1);
                auto label = fmt::format("Lf{}.tmp{}", fn->fid, id);
                as.labels[abs] = label;
            }
        }
        as.offset += insn.length;
    }

    // generate assembly
    as.offset = 0;
    while (ZYAN_SUCCESS(ZydisDecoderDecodeBuffer(decoder, data + as.offset, length - as.offset,
                                                 &insn))) {
        if (auto label = as.labels.get(as.offset)) {
            fmt::print(stream, "{}:\n", *label);
        }
        if (!generate_insn_asm(ctx, fn, &as, stream)) {
            fmt::print(stream, "\t");
            as.fn_call = nullptr;
            char buffer[256];
            ZydisFormatterFormatInstruction(formatter, &insn, buffer, sizeof(buffer),
                                            0);
            fputs(buffer, stream);
            fputc('\n', stream);
        }
        as.offset += insn.length;
    }
}

void Builder::build_binary(AotCompilation* ctx) {
    static const auto exec_header = ".globl main\n";

    ZydisDecoderInit(&ctx->decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_ADDRESS_WIDTH_64);
    ZydisFormatterInit(&ctx->formatter, ZYDIS_FORMATTER_STYLE_ATT);

    string build_name = "chi_build";
    auto asm_path = get_tmp_file_path(build_name + ".s");
    auto asm_out = fopen(asm_path.c_str(), "w");
    if (!asm_out) {
        panic("unable to create temporary assembly file at {}\n", asm_path);
    }
    fflush(asm_out);
    fmt::print(asm_out, exec_header);

    auto jctx = ctx->compiler->get_context();
    // compile functions
    auto& functions = jctx->functions;
    auto max_opt = jit_function_get_max_optimization_level();
    for (auto& fn: functions) {
        fn->set_optimization_level(max_opt);
        fn->build_start();
        fn->build();
        fn->compile();
        fn->build_end();
        ctx->symbol_names[(int64_t) fn->closure()] = fn->get_asm_name();
//        jit_dump_function(stdout, fn->raw(), fn->get_jit_name());
    }

    jit_function entry_fn(jctx->jit_ctx, jit_type_create_signature(jit_abi_cdecl, jit_type_sys_int, nullptr, 0, 1));
    entry_fn.build_start();
    entry_fn.insn_call("main", ctx->entry_fn->raw(), ctx->entry_fn->signature(), nullptr, 0);
    entry_fn.insn_return(entry_fn.new_constant(0));
    entry_fn.compile();
    entry_fn.build_end();

    // string literals
    int sid = 0;
    for (auto str: jctx->string_literals) {
        auto label = fmt::format("L_.str{}", ++sid);
        ctx->add_symbol_name((void*) str, label);
        fmt::print(asm_out, "{}:\n.asciz \"{}\"\n", label, get_strlit_repr(str));
    }

    fmt::print(asm_out, "\t.section\t__DATA,__data\n");
    // type info
    for (auto& type: m_types) {
        if (type->is_placeholder || type->kind == TypeKind::TypeSymbol) {
            continue;
        }
        auto info = ctx->compiler->get_type_info(type.get());
        string name_label;
        if (type->name) {
            name_label = fmt::format("L_.ty{}_name", type->id);
            fmt::print(asm_out, "{}:\n.asciz \"{}\"\n", name_label, info->name);
        }
        auto info_label = fmt::format("L_.ty{}", type->id);
        ctx->add_symbol_name(info, info_label);
        fmt::print(asm_out, "{}:\n", info_label);
        fmt::print(asm_out, "{} {}\n", ASM_NINT_DT, type->name ? name_label : "0x0");
        fmt::print(asm_out, "{} {:#x} #{}\n", ASM_NINT_DT, (int32_t) type->kind, PRINT_ENUM(type->kind));
    }

    //vtable info
    int iid = 0;
    for (auto& impl: jctx->impls) {
        auto impl_label = fmt::format("L_.impl{}", ++iid);
        auto vtable_label = fmt::format("L_.vtable{}", iid);
        fmt::print(asm_out, "{}:\n", vtable_label);
        for (int32_t i = 0; i < impl->vtable_size; i++) {
            auto fn = (jit::Function*) (impl->vtable[i]);
            fmt::print(asm_out, "{} {}\n", ASM_NINT_DT, fn->get_asm_name());
        }
        fmt::print(asm_out, "{}:\n", impl_label);
        fmt::print(asm_out, "{} {}\n", ASM_NINT_DT, ctx->symbol_names[int64_t(impl->type)]);
        fmt::print(asm_out, "{} {}\n", ASM_NINT_DT, vtable_label);
        fmt::print(asm_out, ".word {:#x}\n", impl->vtable_size);
        ctx->add_symbol_name(impl.get(), impl_label);
    }

    // output assembly
    fmt::print(asm_out, "\t.section\t__TEXT,__text,regular,pure_instructions\n");
    array<ZyanU8> buf;
    int fid = 0;
    for (auto& fn: functions) {
        fmt::print(asm_out, "{}:\n", fn->get_asm_name());
        build_fn_asm(ctx, asm_out, ++fid, fn->raw());
    }
    fmt::print(asm_out, "main:\n");
    build_fn_asm(ctx, asm_out, 0, entry_fn.raw());

    // build the final executable
    fclose(asm_out);
    auto as_cmd = fmt::format("as {} -o {}.o", asm_path, asm_path);
    system(as_cmd.c_str());
    auto gcc_cmd = fmt::format("g++ -o {} -lchrt -lfmt {}.o -Wl,-e,main", m_output_file_name, asm_path);
    system(gcc_cmd.c_str());
}

string Builder::get_tmp_file_path(const string& filename) {
#if JIT_WIN32_PLATFORM
    char *tmp_dir = getenv("TMP");
    if(!tmp_dir)
    {
        tmp_dir = getenv("TEMP");
        if(!tmp_dir)
        {
            tmp_dir = "c:/tmp";
        }
    }
    return fmt::format("{}/{}", tmp_dir, filename);
#else
    return fmt::format("/tmp/{}", filename);
#endif
}

void Builder::build_fn_asm(AotCompilation* ctx, FILE* stream, int fid, jit_function_t fn) {
    array<ZyanU8> buf;
    buf.size = 0;
    void* start;
    void* end;
    jit_dump_get_function_entry(fn, &start, &end);
    auto pc = (unsigned char*) start;
    while (pc < (unsigned char*) end) {
        buf.add((ZyanU8) (*pc));
        ++pc;
    }
    AotFunctionInput input;
    input.instructions = &buf;
    input.fid = fid;
    generate_fn_asm(ctx, &input, stream);
}