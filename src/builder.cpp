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

ChiType* Builder::create_type(TypeId type) {
    return m_types.emplace(new ChiType(type))->get();
}

void Builder::set_build_mode(BuildMode value) {
    m_build_mode = value;
    if (value != BuildMode::Run) {
        m_ctx.jit_ctx->enable_aot_compilation(true);
    }
}

void Builder::generate_fn_asm(AotCompilation* ctx, AotFnInput* input, FILE* stream) {
    // Initialize decoder context
    ZydisDecoder decoder;

    ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_ADDRESS_WIDTH_64);

    // Initialize formatter. Only required when you actually plan to do instruction
    // formatting ("disassembling"), like we do here
    ZydisFormatter formatter;
    ZydisFormatterInit(&formatter, ZYDIS_FORMATTER_STYLE_ATT);

    // Loop over the instructions in our buffer.
    ZyanU64 runtime_address = 0;
    ZyanUSize offset = 0;
    auto data = input->instructions->items;
    const ZyanUSize length = input->instructions->size;
    ZydisDecodedInstruction instruction;
    string* fn_name = nullptr;
    map<ZyanUSize, string> labels;

    // scan for labels
    while (ZYAN_SUCCESS(ZydisDecoderDecodeBuffer(&decoder, data + offset, length - offset,
                                                 &instruction))) {
        if (instruction.meta.category == ZYDIS_CATEGORY_COND_BR || instruction.mnemonic == ZYDIS_MNEMONIC_JMP) {
            auto rel = instruction.operands[0].imm.value.s;
            auto abs = offset + instruction.length + rel;
            if (!labels.get(abs)) {
                auto id = int(labels.data.size() + 1);
                auto label = fmt::format("Lf{}tmp{}", input->fid, id);
                labels[abs] = label;
            }
        }
        offset += instruction.length;
    }

    offset = 0;
    // generate code
    while (ZYAN_SUCCESS(ZydisDecoderDecodeBuffer(&decoder, data + offset, length - offset,
                                                 &instruction))) {
        if (auto label = labels.get(offset)) {
            fmt::print(stream, "\t{}:\n", *label);
        }
        bool skip = false;
        if (instruction.mnemonic == ZYDIS_MNEMONIC_MOV) {
            auto& dest = instruction.operands[0];
            auto& src = instruction.operands[1];
            if (dest.type == ZYDIS_OPERAND_TYPE_REGISTER && dest.reg.value == ZYDIS_REGISTER_R11) {
                auto fid = src.imm.value.s;
                fn_name = ctx->symbol_names.get(fid);
                if (fn_name) {
                    skip = true;
                }
            } else if (src.type == ZYDIS_OPERAND_TYPE_IMMEDIATE) {
                auto value = src.imm.value.s;
                char buffer[256];
                ZydisFormatterFormatOperand(&formatter, &instruction, 0, buffer, sizeof(buffer), runtime_address);
                if (auto name = ctx->symbol_names.get(value)) {
                    fmt::print(stream, "\tleaq {}(%rip), {}\n", *name, buffer);
                    skip = true;
                }
            }

        } else if (instruction.mnemonic == ZYDIS_MNEMONIC_MOVSXD) {
            instruction.mnemonic = ZYDIS_MNEMONIC_MOVSX;
//            skip = true;
//            for (int i = 0; i < instruction.length; i++) {
//                fmt::print(stream, ".byte {}\n", data[offset + i]);
//            }
        } else if (instruction.meta.category == ZYDIS_CATEGORY_COND_BR || instruction.mnemonic == ZYDIS_MNEMONIC_JMP) {
            auto rel = instruction.operands[0].imm.value.s;
            auto abs = offset + instruction.length + rel;
            string label = labels[abs];
            fmt::print(stream, "\t{} {}\n", ZydisMnemonicGetString(instruction.mnemonic), label);
            skip = true;

        } else if (instruction.mnemonic == ZYDIS_MNEMONIC_CALL && fn_name) {
            fmt::print(stream, "\tcallq {}\n", *fn_name);
            skip = true;
        }

        if (!skip) {
            fmt::print(stream, "\t");
            fn_name = nullptr;
            char buffer[256];
            ZydisFormatterFormatInstruction(&formatter, &instruction, buffer, sizeof(buffer),
                                            runtime_address);
            fputs(buffer, stream);
            fputc('\n', stream);
        }
        offset += instruction.length;
        runtime_address += instruction.length;
    }
}

void Builder::build_binary(jit::Compiler* compiler) {
    static const auto exec_header = ".globl _main\n";
    AotCompilation ctx;

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
        AotFnInput input;
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
