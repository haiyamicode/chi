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
            jitc.compile_fn(node);
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
    jitc.compile(module);

    switch (m_build_mode) {
        case BuildMode::Run: {
            auto& main_fn = jitc.get_context()->fn_by_node[package->entry_fn];
            main_fn->apply(NULL, NULL);
            break;
        }
        case BuildMode::Executable: {
            build_binary(&jitc);
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
    return m_types.emplace(new ChiType(kind))->get();
}

void Builder::set_build_mode(BuildMode value) {
    m_build_mode = value;
    if (value != BuildMode::Run) {
        m_ctx.jit_ctx->enable_aot_compilation(true);
    }
}

bool Builder::generate_insn_asm(AotCompilation* ctx, AotFunctionInput* input, AssemblyState* as, FILE* stream) {
    auto& insn = as->instruction;
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
                fmt::print(stream, "\tleaq {}(%rip), {}\n", *name, buffer);
                return true;
            }
        }

    } else if (insn.mnemonic == ZYDIS_MNEMONIC_MOVSXD) {
        insn.mnemonic = ZYDIS_MNEMONIC_MOVSX;
        return false;

    } else if (insn.meta.category == ZYDIS_CATEGORY_COND_BR || insn.mnemonic == ZYDIS_MNEMONIC_JMP) {
        auto rel = insn.operands[0].imm.value.s;
        auto abs = as->offset + insn.length + rel;
        string label = as->labels[abs];
        fmt::print(stream, "\t{} {}\n", ZydisMnemonicGetString(insn.mnemonic), label);
        return true;

    } else if (insn.mnemonic == ZYDIS_MNEMONIC_CALL && as->fn_call) {
        fmt::print(stream, "\tcallq {}\n", *as->fn_call);
        return true;
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

void Builder::build_binary(jit::Compiler* compiler) {
    static const auto exec_header = ".globl _main\n";

    AotCompilation ctx;
    ZydisDecoderInit(&ctx.decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_ADDRESS_WIDTH_64);
    ZydisFormatterInit(&ctx.formatter, ZYDIS_FORMATTER_STYLE_ATT);

    string build_name = "chi_build";
    auto asm_path = get_tmp_file_path(build_name + ".s");
    auto asm_out = fopen(asm_path.c_str(), "w");
    if (!asm_out) {
        panic("unable to create temporary assembly file at {}\n", asm_path);
    }
    fflush(asm_out);
    fmt::print(asm_out, exec_header);

    auto jctx = compiler->get_context();
    // compile functions
    auto& functions = jctx->functions;
    auto max_opt = jit_function_get_max_optimization_level();
    for (auto& fn: functions) {
        fn->set_optimization_level(max_opt);
        fn->build_start();
        fn->build();
        fn->compile();
        fn->build_end();
        ctx.symbol_names[(int64_t) fn->closure()] = fn->get_asm_name();
//        jit_dump_function(stdout, fn->raw(), fn->get_jit_name());
    }

    // add native functions to dict
    int fid = 0;
    for (auto& fn_name: jctx->fn_symbols) {
        ctx.symbol_names[--fid] = fn_name;
    }
    int sid = 0;
    for (auto& str: jctx->string_literals) {
        auto name = fmt::format("L_.str{}", ++sid);
        ctx.symbol_names[(int64_t) str] = name;
        fmt::print(asm_out, "{}:\n.asciz \"{}\"\n", name, get_strlit_repr(str));
    }

    // output assembly
    array<ZyanU8> buf;
    fid = 0;
    for (auto& fn: functions) {
        buf.size = 0;
        void* start;
        void* end;
        jit_dump_get_function_entry(fn->raw(), &start, &end);
        auto pc = (unsigned char*) start;
        while (pc < (unsigned char*) end) {
            buf.add((ZyanU8) (*pc));
            ++pc;
        }
        fmt::print(asm_out, "{}:\n", fn->get_asm_name());
        AotFunctionInput input;
        input.instructions = &buf;
        input.fid = ++fid;
        generate_fn_asm(&ctx, &input, asm_out);
    }

    // build the final executable
    fclose(asm_out);
    auto as_cmd = fmt::format("as {} -o {}.o", asm_path, asm_path);
    system(as_cmd.c_str());
    auto gcc_cmd = fmt::format("g++ -o {} -lchrt -lfmt {}.o", m_output_file_name, asm_path);
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
