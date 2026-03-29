/*
 * Copyright (c) 2018 Hai Thanh Nguyen
 *
 * This file is part of chi, which is MIT licensed.
 * See http://opensource.org/licenses/MIT
 */

#include "parser.h"
#include "ast.h"
#include "errors.h"
#include "fmt/core.h"
#include "lexer.h"

using namespace cx;
using namespace cx::ast;

const int LOWEST_PREC = -1;
const int COMMA_PREC = 0;
const int DEFAULT_PREC = COMMA_PREC + 1;
const int TERNARY_PREC = 1;
const int UNARY_PREC = 8; // For parsing unary operator operands

static string get_token_type_repr(TokenType token_type) {
    switch (token_type) {
    case TokenType::IDEN:
        return "an identifier";
    case TokenType::CHAR:
        return "character";
    case TokenType::STRING:
        return "string literal";
    case TokenType::C_STRING:
        return "c-string literal";
    case TokenType::BOOL:
        return "bool literal";
    case TokenType::NULLP:
        return "null literal";
    case TokenType::INT:
        return "int literal";
    case TokenType::FLOAT:
        return "float literal";
    default:
        return fmt::format("'{}'", get_token_symbol(token_type));
    }
}

Parser::Parser(ParseContext *ctx) {
    m_ctx = ctx;
    m_eof_token = m_ctx->allocator->create_token();
}

void Parser::parse() { m_ctx->module->root = parse_root(); }

int Parser::get_op_precedence(TokenType op_type) {
    switch (op_type) {
    case TokenType::COMMA:
        return COMMA_PREC;

    case TokenType::LOR:
        return 2;

    case TokenType::LAND:
        return 3;

    case TokenType::EQ:
    case TokenType::NE:
    case TokenType::LT:
    case TokenType::LE:
    case TokenType::GT:
    case TokenType::GE:
        return 4;

    case TokenType::ADD:
    case TokenType::SUB:
    case TokenType::OR:
    case TokenType::XOR:
        return 5;

    case TokenType::MUL:
    case TokenType::DIV:
    case TokenType::MOD:
    case TokenType::LSHIFT:
    case TokenType::RSHIFT:
    case TokenType::AND:
        return 6;

    case TokenType::KW_AS:
        return 7; // Higher than mul/div, but lower than unary/postfix operators

    default:
        return LOWEST_PREC;
    }
}

Token *Parser::expect(TokenType expected) {
    auto token = read();
    if (token->type == expected) {
        return token;
    } else {
        unread();
        expected_got(expected, token);
        return token;
    }
}

Token *Parser::expect_identifier() {
    auto token = read();
    switch (token->type) {
    case TokenType::IDEN:
    case TokenType::KW_THIS:
    case TokenType::KW_THIS_TYPE:
    case TokenType::KW_NEW:
    case TokenType::KW_DELETE:
    case TokenType::KW_CATCH:
    case TokenType::KW_ASYNC:
    case TokenType::KW_MUT:
        if (token->type == TokenType::KW_ASYNC) {
            token->type = TokenType::IDEN;
        } else if (token->type == TokenType::KW_MUT) {
            token->type = TokenType::IDEN;
        }
        return token;

    default:
        unread();
        expected_got(TokenType::IDEN, token);
        return token;
    }
}

void Parser::expected_got(TokenType expected, Token *token) {
    auto expected_str = get_token_type_repr(expected);
    error(token, errors::TOKEN_EXPECTED_GOT, expected_str, token->to_string());
}

void Parser::unexpected(Token *token) {
    error(token, errors::TOKEN_UNEXPECTED, token->to_string());
    consume();
}

// Error recovery mechanisms
bool Parser::is_statement_start(TokenType type) {
    switch (type) {
    case TokenType::KW_VAR:
    case TokenType::KW_LET:
    case TokenType::KW_IF:
    case TokenType::KW_FOR:
    case TokenType::KW_WHILE:
    case TokenType::KW_RETURN:
    case TokenType::KW_BREAK:
    case TokenType::KW_CONTINUE:
    case TokenType::LBRACE:
    case TokenType::KW_UNSAFE:
    case TokenType::SEMICOLON:
        return true;
    default:
        return false;
    }
}

bool Parser::is_declaration_start(TokenType type) {
    switch (type) {
    case TokenType::KW_FUNC:
    case TokenType::KW_STRUCT:
    case TokenType::KW_UNION:
    case TokenType::KW_INTERFACE:
    case TokenType::KW_ENUM:
    case TokenType::KW_VAR:
    case TokenType::KW_LET:
    case TokenType::KW_CONST:
    case TokenType::KW_EXTERN:
    case TokenType::KW_IMPORT:
    case TokenType::KW_EXPORT:
    case TokenType::KW_TYPEDEF:
    case TokenType::KW_PRIVATE:
    case TokenType::KW_PROTECTED:
    case TokenType::KW_STATIC:
    case TokenType::KW_MUTEX:
    case TokenType::KW_ASYNC:
    case TokenType::KW_UNSAFE:
    case TokenType::AT:
        return true;
    default:
        return false;
    }
}

bool Parser::is_synchronization_point(TokenType type) {
    return is_declaration_start(type) || is_statement_start(type) || type == TokenType::RBRACE ||
           type == TokenType::END;
}

void Parser::recover_to_statement_boundary() {
    while (true) {
        auto token = get();
        if (token->type == TokenType::END || is_statement_start(token->type)) {
            break;
        }
        consume();
    }
}

void Parser::recover_to_declaration_boundary() {
    while (true) {
        auto token = get();
        if (token->type == TokenType::END || is_declaration_start(token->type)) {
            break;
        }
        consume();
    }
}

void Parser::recover_to_synchronization_point() {
    while (true) {
        auto token = get();
        if (token->type == TokenType::END || is_synchronization_point(token->type)) {
            break;
        }
        consume();
    }
}

Token *Parser::next() {
    if (m_toki >= m_ctx->tokens.len) {
        return m_eof_token;
    }
    return m_ctx->tokens.at(m_toki++);
}

Token *Parser::read() { return next(); }

void Parser::jump_to(size_t pos) { m_toki = pos; }

void Parser::unread() { m_toki--; }

Token *Parser::get() { return lookahead(0); }

Token *Parser::lookahead(int n) {
    auto toki = m_toki + n;
    if (toki < 0 || toki >= m_ctx->tokens.len) {
        return m_eof_token;
    }
    return m_ctx->tokens.at(toki);
}

bool Parser::at_comma(TokenType end_token) {
    auto token = get();
    if (token->type == TokenType::COMMA) {
        return true;
    } else if (token->type == end_token) {
        return false;
    }
    unexpected(token);
    return true;
}

Node *Parser::create_node(NodeType type, Token *token) {
    auto node = m_ctx->allocator->create_node(type);
    node->token = token;
    node->module = m_ctx->module;
    if (token->type == TokenType::IDEN || token->type == TokenType::KW_THIS ||
        token->type == TokenType::KW_THIS_TYPE) {
        node->name = token->str;
        if (!token->node) {
            token->node = node;
        }
    }
    return node;
}

Node *Parser::create_identifier_node(Token *iden, Node *decl) {
    auto node = create_node(NodeType::Identifier, iden);
    node->data.identifier.decl = decl;
    node->data.identifier.decl_is_provisional = decl != nullptr;
    if (decl) {
        node->data.identifier.kind = get_identifier_kind(decl);
    }
    return node;
}

Node *Parser::create_unary_expr_node(Token *token) {
    auto node = create_node(NodeType::UnaryOpExpr, token);
    node->data.unary_op_expr.op_type = token->type;
    return node;
}

Node *Parser::create_error_node() { return create_node(NodeType::Error, read()); }

Node *Parser::parse_root() {
    auto node = create_node(NodeType::Root, get());
    this->m_ctx->module->root = node;
    parse_top_level_decls(&node->data.root.top_level_decls);
    return node;
}

void Parser::parse_top_level_decls(NodeList *decls) {
    // first pass, skip all function and struct blocks
    for (;;) {
        auto token = get();
        if (token->type == TokenType::END) {
            break;
        }

        // Skip unexpected tokens and recover to declaration boundary
        if (!is_declaration_start(token->type) && token->type != TokenType::KW_INLINE &&
            token->type != TokenType::KW_STATIC &&
            token->type != TokenType::KW_MUTEX) {
            unexpected(token);
            recover_to_declaration_boundary();
            continue;
        }

        if (token->type == TokenType::KW_INLINE || token->type == TokenType::KW_STATIC) {
            consume();
        }
        auto decl = parse_top_level_decl();
        if (decl) {
            decls->add(decl);
        }

        // add export if exported
        if (decl->type == NodeType::FnDef) {
            if (decl->data.fn_def.decl_spec->is_exported()) {
                m_ctx->module->exports.add(decl);
            }
        } else if (decl->type == NodeType::StructDecl) {
            if (decl->data.struct_decl.decl_spec->is_exported()) {
                m_ctx->module->exports.add(decl);
            }
        } else if (decl->type == NodeType::VarDecl) {
            auto ds = decl->data.var_decl.decl_spec;
            if (ds && ds->is_exported()) {
                m_ctx->module->exports.add(decl);
            }
        } else if (decl->type == NodeType::EnumDecl) {
            if (decl->data.enum_decl.decl_spec->is_exported()) {
                m_ctx->module->exports.add(decl);
            }
        } else if (decl->type == NodeType::TypedefDecl) {
            m_ctx->module->exports.add(decl);
        }
    }

    // second pass, parse function and struct blocks
    for (auto decl : *decls) {
        if (decl->type == NodeType::FnDef) {
            jump_to(m_block_pos[decl]);
            parse_fn_block(decl);
        } else if (decl->type == NodeType::StructDecl) {
            jump_to(m_block_pos[decl]);
            parse_struct_block(decl);
        } else if (decl->type == NodeType::EnumDecl) {
            jump_to(m_block_pos[decl]);
            parse_enum_block(decl);
        }
    }
}

DeclSpec *Parser::parse_decl_spec(DeclSpec *spec) {
    if (!spec) {
        spec = m_ctx->allocator->create_decl_spec();
    }
    parse_attributes(&spec->attributes);
    bool found = true;
    while (found) {
        auto token = get();
        switch (token->type) {
        case TokenType::KW_PRIVATE: {
            consume();
            spec->flags |= DECL_PRIVATE;
            break;
        }
        case TokenType::KW_PROTECTED: {
            consume();
            spec->flags |= DECL_PROTECTED;
            break;
        }
        case TokenType::KW_MUT: {
            consume();
            spec->flags |= DECL_MUTABLE;
            break;
        }
        case TokenType::KW_MUTEX: {
            consume();
            spec->flags |= DECL_MUTEX;
            break;
        }
        case TokenType::KW_STATIC: {
            consume();
            spec->flags |= DECL_STATIC;
            break;
        }
        case TokenType::KW_ASYNC: {
            consume();
            spec->flags |= DECL_ASYNC;
            break;
        }
        case TokenType::KW_UNSAFE: {
            consume();
            spec->flags |= DECL_UNSAFE;
            break;
        }
        default:
            found = false;
            break;
        }
    }
    return spec;
}

void Parser::parse_attributes(NodeList *attributes) {
    while (next_is(TokenType::AT)) {
        attributes->add(parse_attribute());
    }
}

Node *Parser::parse_attribute() {
    auto at = expect(TokenType::AT);
    expect(TokenType::LBRACK);
    auto iden = expect(TokenType::IDEN);
    auto term = create_identifier_node(iden, nullptr);
    while (next_is(TokenType::DOT)) {
        term = parse_dot_expr(term);
    }
    auto node = create_node(NodeType::DeclAttribute, iden);
    node->data.decl_attribute.term = term;
    expect(TokenType::RBRACK);
    return node;
}

Node *Parser::parse_top_level_decl(DeclSpec *decl_spec) {
    auto token = get();
    switch (token->type) {
    case TokenType::AT:
    case TokenType::KW_PRIVATE:
    case TokenType::KW_PROTECTED:
    case TokenType::KW_STATIC:
    case TokenType::KW_MUTEX:
    case TokenType::KW_ASYNC:
    case TokenType::KW_UNSAFE:
        return parse_top_level_decl(parse_decl_spec());
    case TokenType::KW_STRUCT:
    case TokenType::KW_UNION:
    case TokenType::KW_INTERFACE: {
        return parse_struct_decl(token->type, decl_spec);
    }
    case TokenType::KW_ENUM:
        return parse_enum_decl(decl_spec);
    case TokenType::KW_TYPEDEF:
        return parse_typedef();
    case TokenType::KW_VAR:
    case TokenType::KW_LET:
    case TokenType::KW_CONST:
        return parse_var_decl(false, decl_spec);
    case TokenType::KW_FUNC: {
        return parse_fn_decl(FN_BODY_REQUIRED, decl_spec);
    }
    case TokenType::KW_EXTERN:
        return parse_extern_decl(decl_spec);
    case TokenType::KW_IMPORT:
        return parse_import_decl();
    case TokenType::KW_EXPORT: {
        // Peek at the token after 'export':
        //   export * from / export {} from  → re-export statement (parse_export_decl)
        //   export func/struct/let/...      → declaration modifier (DECL_EXPORTED)
        auto next_tok = lookahead(1);
        if (next_tok->type == TokenType::MUL || next_tok->type == TokenType::LBRACE) {
            return parse_export_decl();
        }
        // Pattern export: export IDEN* from "module" (e.g. export SDL_* from "./sdl")
        if (next_tok->type == TokenType::IDEN) {
            auto after_iden = lookahead(2);
            if (after_iden->type == TokenType::MUL) {
                return parse_export_decl();
            }
        }
        consume(); // consume 'export'
        if (!decl_spec) decl_spec = m_ctx->allocator->create_decl_spec();
        decl_spec->flags |= DECL_EXPORTED;
        decl_spec = parse_decl_spec(decl_spec); // collect any additional modifiers
        return parse_top_level_decl(decl_spec);
    }
    default:
        unexpected(token);
        recover_to_declaration_boundary();
        return create_error_node();
    }
}

FnKind Parser::parse_fn_identifier(Token **iden) {
    FnKind fn_kind = FnKind::TopLevel;
    auto parent = get_scope()->owner;
    if (parent && parent->type == NodeType::StructDecl) {
        auto token = get();
        if (token->type == TokenType::KW_DELETE) {
            consume();
            fn_kind = FnKind::Destructor;
            *iden = token;
        } else if (token->type == TokenType::KW_NEW) {
            consume();
            fn_kind = FnKind::Constructor;
            *iden = token;
        } else {
            fn_kind = FnKind::Method;
            *iden = expect_identifier();
        }
    } else {
        *iden = expect(TokenType::IDEN);
    }
    return fn_kind;
}

IdentifierKind Parser::get_identifier_kind(Node *node) {
    switch (node->type) {
    case NodeType::Primitive:
    case NodeType::StructDecl:
    case NodeType::TypedefDecl:
    case NodeType::TypeParam:
        return IdentifierKind::TypeName;
    default:
        return IdentifierKind::Value;
    }
}

Node *Parser::parse_identifier() {
    auto token = expect_identifier();
    auto decl = m_ctx->resolver->find_symbol(token->str);
    auto node = create_identifier_node(token, decl);
    if (!decl && token->type != TokenType::KW_THIS_TYPE && !m_ctx->format_mode &&
        node->name == "char") {
        error(token, errors::CHAR_USE_BYTE);
    }
    if (token->type == TokenType::KW_THIS) {
        node->data.identifier.kind = IdentifierKind::This;
    } else if (token->type == TokenType::KW_THIS_TYPE) {
        node->data.identifier.kind = IdentifierKind::ThisType;
    }
    return node;
}

optional<SigilKind> Parser::get_sigil_kind(TokenType token_type) {
    switch (token_type) {
    case TokenType::MUL:
        return SigilKind::Pointer;
    case TokenType::AND:
        return SigilKind::Reference;
    case TokenType::QUES:
        return SigilKind::Optional;
    default:
        return {};
    }
}

void Parser::parse_reference_type_prefix(SigilKind &sigil_kind, string &lifetime) {
    if (next_is(TokenType::LPAREN)) {
        // &(mut, 'this) T  or  &(mutex) T  or  &('this) T  or  &(move) T
        consume();
        for (;;) {
            if (next_is(TokenType::KW_MUT)) {
                consume();
                sigil_kind = SigilKind::MutRef;
            } else if (next_is(TokenType::KW_MUTEX)) {
                consume();
                sigil_kind = SigilKind::MutexRef;
            } else if (next_is(TokenType::KW_MOVE)) {
                consume();
                sigil_kind = SigilKind::Move;
            } else if (next_is(TokenType::LIFETIME)) {
                lifetime = get()->str;
                consume();
            } else {
                break;
            }
            if (!next_is(TokenType::COMMA))
                break;
            consume();
        }
        expect(TokenType::RPAREN);
    } else if (next_is(TokenType::KW_MUT)) {
        consume();
        sigil_kind = SigilKind::MutRef;
    } else if (next_is(TokenType::KW_MUTEX)) {
        consume();
        sigil_kind = SigilKind::MutexRef;
    } else if (next_is(TokenType::KW_MOVE)) {
        consume();
        sigil_kind = SigilKind::Move;
    } else if (next_is(TokenType::LIFETIME)) {
        lifetime = get()->str;
        consume();
    }
}

bool Parser::try_parse_reference_type_prefix_lookahead(int &pos, SigilKind &sigil_kind) {
    auto token = lookahead(pos);
    if (token->type == TokenType::LPAREN) {
        pos++;
        for (;;) {
            token = lookahead(pos);
            if (token->type == TokenType::KW_MUT || token->type == TokenType::KW_MUTEX ||
                token->type == TokenType::KW_MOVE || token->type == TokenType::LIFETIME) {
                if (token->type == TokenType::KW_MUT) {
                    sigil_kind = SigilKind::MutRef;
                } else if (token->type == TokenType::KW_MUTEX) {
                    sigil_kind = SigilKind::MutexRef;
                } else if (token->type == TokenType::KW_MOVE) {
                    sigil_kind = SigilKind::Move;
                }
                pos++;
            } else {
                break;
            }
            token = lookahead(pos);
            if (token->type != TokenType::COMMA)
                break;
            pos++;
        }
        if (lookahead(pos)->type != TokenType::RPAREN)
            return false;
        pos++;
    } else if (token->type == TokenType::KW_MUT || token->type == TokenType::KW_MUTEX ||
               token->type == TokenType::KW_MOVE) {
        if (token->type == TokenType::KW_MUT) {
            sigil_kind = SigilKind::MutRef;
        } else if (token->type == TokenType::KW_MUTEX) {
            sigil_kind = SigilKind::MutexRef;
        } else if (token->type == TokenType::KW_MOVE) {
            sigil_kind = SigilKind::Move;
        }
        pos++;
    } else if (token->type == TokenType::LIFETIME) {
        pos++;
    }
    return lookahead(pos)->type != TokenType::END;
}

Node *Parser::parse_type_expr(bool type_only) {
    if (!type_only) {
        expect(TokenType::COLON);
    }

    array<Node *> sigil_nodes;
    auto wrap_sigil_nodes = [&](Node *node) {
        for (int i = int(sigil_nodes.len) - 1; i >= 0; --i) {
            auto parent = sigil_nodes[i];
            parent->data.sigil_type.type = node;
            node = parent;
        }
        return node;
    };
    for (;;) {
        auto token = get();
        // [N]T fixed-size array type
        if (token->type == TokenType::LBRACK && lookahead(1)->type == TokenType::INT) {
            consume(); // [
            auto size_token = get();
            uint32_t size = (uint32_t)size_token->val.i;
            consume(); // INT
            expect(TokenType::RBRACK);
            auto node = create_node(NodeType::TypeSigil, token);
            node->data.sigil_type.sigil = SigilKind::FixedArray;
            node->data.sigil_type.fixed_size = size;
            sigil_nodes.add(node);
            continue;
        }
        if (auto sigil_kind = get_sigil_kind(token->type)) {
            consume();

            string lifetime;
            if (sigil_kind == SigilKind::Reference) {
                parse_reference_type_prefix(*sigil_kind, lifetime);
            }

            if ((sigil_kind == SigilKind::Reference || sigil_kind == SigilKind::MutRef) &&
                next_is(TokenType::LBRACK) && lookahead(1)->type != TokenType::INT) {
                consume(); // [
                auto node = create_node(NodeType::TypeSigil, token);
                node->data.sigil_type.sigil = SigilKind::Span;
                node->data.sigil_type.lifetime = lifetime;
                node->data.sigil_type.is_mut = sigil_kind == SigilKind::MutRef;
                node->data.sigil_type.type = parse_type_expr(true);
                expect(TokenType::RBRACK);
                return wrap_sigil_nodes(node);
            }

            auto node = create_node(NodeType::TypeSigil, token);
            node->data.sigil_type.sigil = *sigil_kind;
            node->data.sigil_type.lifetime = lifetime;
            sigil_nodes.add(node);

        } else {
            break;
        }
    }

    ast::Node *node = nullptr;
    auto token = get();
    if (token->type == TokenType::LPAREN) {
        consume();
        node = parse_type_expr(true);
        expect(TokenType::RPAREN);
    } else if (token->type == TokenType::KW_FUNC) {
        consume();
        node = parse_fn_type(token);
    } else {
        // Check if we have a valid identifier token for type
        if (token->type != TokenType::IDEN && token->type != TokenType::KW_THIS &&
            token->type != TokenType::KW_THIS_TYPE && token->type != TokenType::KW_NEW &&
            token->type != TokenType::KW_DELETE) {
            error(token, "expected type identifier, got '{}'", token->to_string());
            return create_error_node();
        }
        auto iden = parse_identifier();
        node = iden;
        if (next_is(TokenType::DOT)) {
            node = parse_dot_expr(iden);
        }

        if (next_is(TokenType::LT)) {
            consume();
            auto base_type_node = node;
            node = create_node(NodeType::SubtypeExpr, iden->token);
            auto &subtype = node->data.subtype_expr;
            subtype.type = base_type_node;
            Token *token;
            for (;;) {
                token = get();
                if (token->type == TokenType::END) {
                    error(token, errors::UNEXPECTED_EOF);
                    return node;
                }
                if (token->type == TokenType::GT) {
                    break;
                }
                // Handle lifetime args: Holder<'a>
                if (token->type == TokenType::LIFETIME) {
                    subtype.lifetime_args.add(parse_lifetime_param(token));
                // Handle ...T spread in type args
                } else if (token->type == TokenType::ELLIPSIS) {
                    consume(); // skip past ...
                    auto param = parse_type_expr(true);
                    auto expansion = create_node(NodeType::PackExpansion, token);
                    expansion->data.pack_expansion.expr = param;
                    subtype.args.add(expansion);
                } else {
                    auto param = parse_type_expr(true);
                    subtype.args.add(param);
                }
                if (!at_comma(TokenType::GT)) {
                    break;
                }
                consume();
            }
            expect(TokenType::GT);
        }

        // Handle dot-qualified access after generic args (e.g., Container<int>.Single)
        if (next_is(TokenType::DOT)) {
            node = parse_dot_expr(node);
        }
    }

    return wrap_sigil_nodes(node);
}

// Lookahead to check if current '(' starts an arrow lambda: (params) [ret_type] =>
// Params are: iden [: type], with optional leading '...' for variadics
bool Parser::is_arrow_lambda_ahead() {
    int pos = 1; // after '('

    // Parse parameter list
    for (;;) {
        auto tok = lookahead(pos);
        if (tok->type == TokenType::END)
            return false;
        if (tok->type == TokenType::RPAREN) {
            pos++;
            break;
        }

        // Optional variadic
        if (tok->type == TokenType::ELLIPSIS) {
            pos++;
            tok = lookahead(pos);
        }

        // Each param must be an identifier
        if (tok->type != TokenType::IDEN)
            return false;
        pos++;

        // Optional ': type'
        tok = lookahead(pos);
        if (tok->type == TokenType::COLON) {
            pos++;
            if (!try_parse_type_expr_lookahead(pos))
                return false;
            tok = lookahead(pos);
        }

        if (tok->type == TokenType::COMMA) {
            pos++;
        } else if (tok->type == TokenType::RPAREN) {
            pos++;
            break;
        } else
            return false;
    }

    // Check for '=>' directly or after a return type
    if (lookahead(pos)->type == TokenType::ARROW)
        return true;
    int type_pos = pos;
    if (try_parse_type_expr_lookahead(type_pos)) {
        if (lookahead(type_pos)->type == TokenType::ARROW)
            return true;
    }
    return false;
}

Node *Parser::parse_fn_lambda() {
    Token *token;
    if (next_is(TokenType::KW_FUNC)) {
        token = expect(TokenType::KW_FUNC);
    } else {
        token = get();
    }
    auto fn = create_node(NodeType::FnDef, token);

    // Parse optional by-value capture list: func [x, y] (params) { ... }
    if (next_is(TokenType::LBRACK)) {
        read(); // consume [
        while (!next_is(TokenType::RBRACK)) {
            auto iden = expect(TokenType::IDEN);
            fn->data.fn_def.value_captures.add(iden->str);
            if (!next_is(TokenType::COMMA))
                break;
            read(); // consume ,
        }
        expect(TokenType::RBRACK);
    }

    auto proto = parse_fn_proto(token, fn);
    fn->name = "";
    fn->data.fn_def.fn_kind = FnKind::Lambda;
    fn->data.fn_def.fn_proto = proto;
    fn->data.fn_def.decl_spec = m_ctx->allocator->create_decl_spec();
    fn->parent_fn =
        get_scope()->find_parent(NodeType::FnDef); // Set parent function for nested lambda chain
    proto->data.fn_proto.fn_def_node = fn;

    // Check for arrow syntax: func(x) => expr
    if (next_is(TokenType::ARROW)) {
        auto arrow = read(); // consume =>

        // => { block } collapses to a regular block body
        if (next_is(TokenType::LBRACE)) {
            parse_fn_block(fn);
        } else {
            // Create scope and add params
            auto scope = m_ctx->resolver->push_scope(fn);
            auto &fn_proto = proto->data.fn_proto;
            for (auto param : fn_proto.params) {
                add_to_scope(param);
                param->parent_fn = fn;
            }

            // Parse expression and wrap in return statement
            auto expr = parse_child_expr_construct(false, fn);
            auto ret = create_node(NodeType::ReturnStmt, arrow);
            ret->data.return_stmt.expr = expr;

            // Create block containing the return statement
            auto block = create_node(NodeType::Block, arrow);
            block->data.block.scope = scope;
            block->data.block.statements.add(ret);
            block->data.block.is_arrow = true;
            fn->data.fn_def.body = block;

            m_ctx->resolver->pop_scope();
        }
    } else {
        parse_fn_block(fn);
    }
    return fn;
}

Node *Parser::parse_fn_decl(uint32_t flags, DeclSpec *decl_spec) {
    decl_spec = parse_decl_spec(decl_spec);
    auto iden = expect(TokenType::KW_FUNC);
    auto kind = parse_fn_identifier(&iden);

    auto fn = create_node(NodeType::FnDef, iden);
    fn->start_token = iden;
    fn->name = iden->get_name();
    auto proto = parse_fn_proto(iden, fn);
    proto->data.fn_proto.fn_def_node = fn;
    fn->data.fn_def.fn_proto = proto;
    fn->data.fn_def.fn_kind = kind;
    fn->data.fn_def.decl_spec = decl_spec;
    fn->data.fn_def.body = nullptr;

    if (flags & FN_BODY_NONE) {
        expect(TokenType::SEMICOLON);
        add_to_scope(fn);
        return fn;
    }

    if (kind == FnKind::TopLevel) {
        save_block_pos(fn);
        skip_block();
    } else {
        if (flags & FN_BODY_REQUIRED) {
            parse_fn_block(fn);
        } else {
            if (next_is(TokenType::SEMICOLON)) {
                consume();
            } else {
                if (next_is(TokenType::LBRACE)) {
                    parse_fn_block(fn);
                }
            }
        }
    }

    fn->end_token = lookahead(-1);
    add_to_scope(fn);
    return fn;
}

void Parser::parse_fn_block(Node *fn) {
    auto scope = m_ctx->resolver->push_scope(fn);
    auto &fn_def = fn->data.fn_def;
    auto &fn_proto = fn_def.fn_proto->data.fn_proto;
    for (auto param : fn_proto.params) {
        add_to_scope(param);
        param->parent_fn = fn;
    }

    // Add type parameters to scope
    for (auto type_param : fn_proto.type_params) {
        m_ctx->resolver->declare_symbol(type_param->name, type_param);
    }
    fn_def.body = parse_block(scope);
    m_ctx->resolver->pop_scope();
}

Node *Parser::parse_var_decl(bool as_field, DeclSpec *decl_spec) {
    decl_spec = parse_decl_spec(decl_spec);
    VarKind var_kind = VarKind::Mutable;
    if (!as_field) {
        if (next_is(TokenType::KW_LET)) {
            var_kind = VarKind::Immutable;
            consume();
        } else if (next_is(TokenType::KW_CONST)) {
            var_kind = VarKind::Constant;
            consume();
        } else {
            expect(TokenType::KW_VAR);
        }
    }

    if (!as_field && starts_destructure_pattern()) {
        return parse_any_destructure_decl(var_kind);
    }

    bool is_embed = false;
    if (as_field && next_is(TokenType::ELLIPSIS)) {
        is_embed = true;
        consume();
        // Interface-style embed: ...TypeName; (no field name, no colon)
        // Parse as interface embed and let resolver reject if in wrong context
        if (next_is(TokenType::IDEN) && lookahead(1)->type != TokenType::COLON) {
            auto embed_node = create_node(NodeType::VarDecl, get());
            embed_node->data.var_decl.is_embed = true;
            embed_node->data.var_decl.is_field = false;
            embed_node->data.var_decl.type = parse_type_expr(true);
            embed_node->name = "__embed";
            expect(TokenType::SEMICOLON);
            return embed_node;
        }
    }
    auto iden = expect(TokenType::IDEN);
    if (iden->type != TokenType::IDEN) {
        return create_error_node();
    }
    auto node = create_node(NodeType::VarDecl, iden);
    node->data.var_decl.identifier = iden;
    node->data.var_decl.kind = var_kind;
    node->data.var_decl.is_field = as_field;
    node->data.var_decl.decl_spec = decl_spec;
    node->data.var_decl.is_embed = is_embed;

    if (!as_field) {
        node->parent_fn = get_scope()->find_parent(NodeType::FnDef);
        // Allow top-level const and let declarations, but not var outside functions
        if (!node->parent_fn && var_kind == VarKind::Mutable) {
            error(iden, "global variables are not supported; use 'let' or 'const' for module-level "
                        "declarations");
        }
    }
    if (!next_is(TokenType::ASS)) {
        auto token = get();
        if (token->type == TokenType::END || token->type == TokenType::RBRACE ||
            token->type == TokenType::SEMICOLON) {
            // Missing type - create error node but continue parsing
            error(iden, "missing type declaration for variable '{}'", iden->str);
            node->data.var_decl.type = create_error_node();
        } else {
            node->data.var_decl.type = parse_type_expr();
        }
    }
    if (next_is(TokenType::ASS)) {
        consume();
        node->data.var_decl.expr = parse_child_expr_construct(false, node);
        node->data.var_decl.initialized_at = node;
    }
    expect(TokenType::SEMICOLON);
    auto so = get_scope()->owner;
    if (!so || so->type != NodeType::StructDecl) {
        add_to_scope(node);
    }
    return node;
}

bool Parser::starts_destructure_pattern() {
    return next_is(TokenType::LBRACE) || next_is(TokenType::LBRACK) ||
           next_is(TokenType::LPAREN);
}

Node *Parser::parse_destructure_pattern(VarKind kind) {
    auto lbrace = expect(TokenType::LBRACE);
    auto node = create_node(NodeType::DestructureDecl, lbrace);
    node->data.destructure_decl.kind = kind;

    for (;;) {
        auto token = get();
        if (token->type == TokenType::RBRACE || token->type == TokenType::END)
            break;

        auto sigil = SigilKind::None;
        if (next_is(TokenType::AND)) {
            consume();
            if (next_is(TokenType::KW_MUT)) {
                consume();
                sigil = SigilKind::MutRef;
            } else if (next_is(TokenType::KW_MUTEX)) {
                consume();
                sigil = SigilKind::MutexRef;
            } else {
                sigil = SigilKind::Reference;
            }
        }

        auto field_token = expect(TokenType::IDEN);
        if (field_token->type != TokenType::IDEN)
            break;

        auto field_node = create_node(NodeType::DestructureField, field_token);
        field_node->data.destructure_field.field_name = field_token;
        field_node->data.destructure_field.binding_name = field_token; // default: same name
        field_node->data.destructure_field.sigil = sigil;

        if (next_is(TokenType::COLON)) {
            consume(); // consume ':'
            if (starts_destructure_pattern()) {
                field_node->data.destructure_field.nested = parse_any_destructure_pattern(kind);
                field_node->data.destructure_field.binding_name = nullptr;
            } else {
                auto binding_token = expect(TokenType::IDEN);
                field_node->data.destructure_field.binding_name = binding_token;
            }
        }

        node->data.destructure_decl.fields.add(field_node);
        field_node->parent = node;

        if (!at_comma(TokenType::RBRACE))
            break;
        consume(); // consume ','
    }
    expect(TokenType::RBRACE);
    return node;
}

static void register_destructure_bindings(Parser *parser, ast::Node *pattern) {
    for (auto field_node : pattern->data.destructure_decl.fields) {
        auto &fd = field_node->data.destructure_field;
        if (fd.nested) {
            register_destructure_bindings(parser, fd.nested);
        } else if (fd.binding_name) {
            // Create a placeholder VarDecl so subsequent parsing can find the identifier
            auto var = parser->create_node(NodeType::VarDecl, fd.binding_name);
            var->name = fd.binding_name->get_name();
            var->data.var_decl.kind = pattern->data.destructure_decl.kind;
            var->data.var_decl.identifier = fd.binding_name;
            parser->add_to_scope(var);
        }
    }
}

Node *Parser::parse_sequence_destructure_pattern(VarKind kind, TokenType start,
                                              TokenType end, bool is_array) {
    auto token = expect(start);
    auto node = create_node(NodeType::DestructureDecl, token);
    node->data.destructure_decl.kind = kind;
    node->data.destructure_decl.is_array = is_array;
    node->data.destructure_decl.is_tuple = !is_array;

    for (;;) {
        token = get();
        if (token->type == end || token->type == TokenType::END)
            break;

        bool is_rest = false;
        if (next_is(TokenType::ELLIPSIS)) {
            consume();
            is_rest = true;
        }

        auto sigil = SigilKind::None;
        if (!is_rest && next_is(TokenType::AND)) {
            consume();
            if (next_is(TokenType::KW_MUT)) {
                consume();
                sigil = SigilKind::MutRef;
            } else if (next_is(TokenType::KW_MUTEX)) {
                consume();
                sigil = SigilKind::MutexRef;
            } else {
                sigil = SigilKind::Reference;
            }
        }

        auto binding_token = expect(TokenType::IDEN);
        if (binding_token->type != TokenType::IDEN)
            break;

        auto field_node = create_node(NodeType::DestructureField, binding_token);
        field_node->data.destructure_field.field_name = binding_token;
        field_node->data.destructure_field.binding_name = binding_token;
        field_node->data.destructure_field.sigil = sigil;
        field_node->data.destructure_field.is_rest = is_rest;

        node->data.destructure_decl.fields.add(field_node);
        field_node->parent = node;

        if (is_rest)
            break;
        if (!at_comma(end))
            break;
        consume();
    }
    expect(end);
    return node;
}

Node *Parser::parse_any_destructure_pattern(VarKind kind) {
    if (next_is(TokenType::LBRACE))
        return parse_destructure_pattern(kind);
    if (next_is(TokenType::LBRACK))
        return parse_array_destructure_pattern(kind);
    if (next_is(TokenType::LPAREN))
        return parse_tuple_destructure_pattern(kind);
    error(get(), "expected destructure pattern");
    return create_error_node();
}

Node *Parser::parse_any_destructure_decl(VarKind kind) {
    auto node = parse_any_destructure_pattern(kind);
    node->parent_fn = get_scope()->find_parent(NodeType::FnDef);
    expect(TokenType::ASS);
    node->data.destructure_decl.expr = parse_child_expr_construct(false, node);
    expect(TokenType::SEMICOLON);

    register_destructure_bindings(this, node);
    return node;
}

Node *Parser::parse_destructure_decl(VarKind kind) { return parse_any_destructure_decl(kind); }

Node *Parser::parse_array_destructure_pattern(VarKind kind) {
    return parse_sequence_destructure_pattern(kind, TokenType::LBRACK, TokenType::RBRACK,
                                              true);
}

Node *Parser::parse_array_destructure_decl(VarKind kind) {
    return parse_any_destructure_decl(kind);
}

Node *Parser::parse_tuple_destructure_pattern(VarKind kind) {
    return parse_sequence_destructure_pattern(kind, TokenType::LPAREN, TokenType::RPAREN,
                                              false);
}

Node *Parser::parse_tuple_destructure_decl(VarKind kind) {
    return parse_any_destructure_decl(kind);
}

Node *Parser::parse_fn_proto(Token *token, Node *fn_node) {
    auto proto = create_node(NodeType::FnProto, token);
    proto->name = token->get_name();

    // Push a scope for function prototype parsing
    auto proto_scope = m_ctx->resolver->push_scope(proto);

    // Parse type parameters and lifetime parameters like <'a, 'b: 'a, T, U: Show>
    auto &type_params = proto->data.fn_proto.type_params;
    auto &lifetime_params = proto->data.fn_proto.lifetime_params;
    if (next_is(TokenType::LT)) {
        expect(TokenType::LT);
        Token *param_token;
        for (;;) {
            param_token = get();
            if (param_token->type == TokenType::END) {
                error(param_token, errors::UNEXPECTED_EOF);
                m_ctx->resolver->pop_scope();
                return proto;
            }
            if (param_token->type == TokenType::GT) {
                break;
            }

            if (param_token->type == TokenType::LIFETIME) {
                auto lt_node = parse_lifetime_param(param_token);
                lt_node->data.lifetime_param.index = lifetime_params.len;
                lt_node->data.lifetime_param.source_decl = fn_node;
                lifetime_params.add(lt_node);
            } else {
                // Type parameter: T or ...T (variadic) or T: SomeInterface
                bool is_variadic = token->type == TokenType::ELLIPSIS;
                if (is_variadic) consume(); // skip past ...
                auto param_iden = expect(TokenType::IDEN);
                auto param_node = create_node(NodeType::TypeParam, param_iden);
                param_node->name = param_iden->str;
                param_node->data.type_param.index = type_params.len;
                param_node->data.type_param.source_decl = fn_node;
                param_node->data.type_param.is_variadic = is_variadic;

                if (next_is(TokenType::COLON)) {
                    consume();
                    if (next_is(TokenType::LIFETIME)) {
                        // T: 'a — lifetime bound
                        param_node->data.type_param.lifetime_bound = get()->str;
                        consume();
                    } else {
                        // T: Trait1 + Trait2 + ...
                        do {
                            param_node->data.type_param.type_bounds.add(parse_type_expr(true));
                        } while (next_is(TokenType::ADD) && (consume(), true));
                    }
                }

                type_params.add(param_node);
                add_to_scope(param_node);
            }

            if (!at_comma(TokenType::GT)) {
                break;
            }
            consume();
        }
        expect(TokenType::GT);
    }

    expect(TokenType::LPAREN);
    auto vararg = parse_fn_params(&proto->data.fn_proto.params);
    if (vararg) {
        proto->data.fn_proto.is_vararg = true;
    }
    expect(TokenType::RPAREN);
    // Don't parse return type if next is {, ;, or => (arrow for lambda expression body)
    if (!next_is(TokenType::LBRACE) && !next_is(TokenType::SEMICOLON) &&
        !next_is(TokenType::ARROW)) {
        proto->data.fn_proto.return_type = parse_type_expr(true);
    }

    // Pop the prototype scope
    m_ctx->resolver->pop_scope();
    return proto;
}

Node *Parser::parse_lifetime_param(Token *token) {
    consume();
    if (token->str == "this") {
        error(token, "'this is a reserved lifetime and cannot be declared");
    }
    auto lt_node = create_node(NodeType::LifetimeParam, token);
    lt_node->name = token->str;
    if (next_is(TokenType::COLON)) {
        consume();
        auto bound_token = expect(TokenType::LIFETIME);
        lt_node->data.lifetime_param.bound = bound_token->str;
    }
    return lt_node;
}

Node *Parser::parse_fn_type(Token *func) {
    auto proto = create_node(NodeType::FnProto, func);
    auto &data = proto->data.fn_proto;
    data.is_type_expr = true;

    // func<'static> — lifetime params on func type
    if (next_is(TokenType::LT)) {
        consume();
        for (;;) {
            auto token = get();
            if (token->type == TokenType::GT || token->type == TokenType::END)
                break;
            if (token->type == TokenType::LIFETIME) {
                data.lifetime_params.add(parse_lifetime_param(token));
            } else {
                error(token, "expected lifetime parameter, got '{}'", token->to_string());
                break;
            }
            if (!at_comma(TokenType::GT))
                break;
            consume();
        }
        expect(TokenType::GT);
    }

    if (!next_is(TokenType::LPAREN)) {
        return proto;
    }
    expect(TokenType::LPAREN);
    auto vararg = parse_fn_params(&data.params);
    if (vararg) {
        data.is_vararg = true;
    }
    expect(TokenType::RPAREN);
    auto next_is_separator = next_is(TokenType::RPAREN) || next_is(TokenType::SEMICOLON) ||
                             next_is(TokenType::COMMA) || next_is(TokenType::GT) ||
                             next_is(TokenType::ASS); // For default values in struct fields
    if (!next_is_separator) {
        data.return_type = parse_type_expr(true);
    }
    return proto;
}

bool Parser::parse_fn_params(NodeList *params) {
    Token *token;
    for (;;) {
        token = get();
        if (token->type == TokenType::END) {
            error(token, errors::UNEXPECTED_EOF);
            return true;
        }
        if (token->type == TokenType::RPAREN) {
            break;
        }
        if (token->type == TokenType::ELLIPSIS && lookahead(1)->type == TokenType::RPAREN) {
            consume();
            return true;
        }
        auto param = parse_fn_param();
        params->add(param);
        if (!at_comma(TokenType::RPAREN)) {
            break;
        }
        consume();
    }

    // Validate: required params cannot come after optional params
    bool seen_optional = false;
    for (auto param : *params) {
        bool has_default = param->data.param_decl.default_value != nullptr;
        if (has_default) {
            seen_optional = true;
        } else if (seen_optional && !param->data.param_decl.is_variadic) {
            error(param->token, "required parameter cannot follow optional parameter");
        }
    }

    return false;
}

Node *Parser::parse_fn_param() {
    bool is_variadic = false;
    if (next_is(TokenType::ELLIPSIS)) {
        consume();
        is_variadic = true;
    }
    auto iden = expect(TokenType::IDEN);

    // Check if type is provided (colon indicates type annotation)
    auto token = get();
    Node *type = nullptr;
    if (token->type == TokenType::COLON) {
        // Has type annotation
        type = parse_type_expr();
    }
    // If no colon, type remains nullptr - will be inferred during resolution

    auto param = create_node(NodeType::ParamDecl, iden);
    param->data.param_decl.type = type;
    param->data.param_decl.is_variadic = is_variadic;

    // Check for default value
    if (next_is(TokenType::ASS)) {
        consume(); // consume '='
        param->data.param_decl.default_value = parse_child_expr_construct(false, param);
    }

    return param;
}

Node *Parser::parse_block(Scope *scope, Token *arrow) {
    Token *token = arrow;
    bool has_braces = false;
    if (!arrow) {
        token = expect(TokenType::LBRACE);
        has_braces = true;
    } else {
        if (next_is(TokenType::LBRACE)) {
            consume();
            has_braces = true;
        }
    }

    auto node = create_node(NodeType::Block, token);
    node->data.block.has_braces = has_braces;
    node->start_token = token;
    if (arrow) {
        node->data.block.is_arrow = true;
    }
    bool should_pop_scope = false;
    if (!scope) {
        scope = m_ctx->resolver->push_scope(node);
        should_pop_scope = true;
    }
    node->data.block.scope = scope;

    if (!has_braces) {
        auto start_token = token;
        // Use parse_child_expr_construct so T{} is valid in arrow positions (=> T{})
        auto expr = parse_child_expr_construct(false, node);
        node->data.block.return_expr = expr;
        expr->start_token = start_token;
        expr->end_token = lookahead(-1);

    } else {
        for (;;) {
            auto token = get();
            if (token->type == TokenType::END) {
                error(token, errors::UNEXPECTED_EOF);
                break;
            }
            if (token->type == TokenType::RBRACE) {
                consume();
                node->end_token = token;
                break;
            }
            bool as_expr = false;
            auto start_token = token;
            auto stmt = parse_stmt(&as_expr);
            if (stmt) {
                stmt->parent = node;
                stmt->start_token = start_token;
                stmt->end_token = lookahead(-1);
            } else {
                // Failed to parse statement, skip to next statement boundary
                recover_to_statement_boundary();
                continue;
            }

            if (as_expr) {
                node->data.block.return_expr = stmt;
                auto rbrace = expect(TokenType::RBRACE);
                node->end_token = rbrace;
                // node->data.block.statements.add(stmt);
                break;
            } else {
                stmt->index = node->data.block.statements.len;
                node->data.block.statements.add(stmt);
            }
        }
    }
    if (should_pop_scope) {
        m_ctx->resolver->pop_scope();
    }
    return node;
}

static bool block_returns_value(ast::Node *node) {
    return node && node->type == NodeType::Block && node->data.block.return_expr;
}

static bool if_expr_returns_value(ast::Node *node) {
    if (!node || node->type != NodeType::IfExpr) {
        return false;
    }

    auto &data = node->data.if_expr;
    if (!data.else_node || !block_returns_value(data.then_block)) {
        return false;
    }

    if (data.else_node->type == NodeType::Block) {
        return block_returns_value(data.else_node);
    }

    if (data.else_node->type == NodeType::IfExpr) {
        return if_expr_returns_value(data.else_node);
    }

    return false;
}

static bool switch_expr_returns_value(ast::Node *node) {
    if (!node || node->type != NodeType::SwitchExpr) {
        return false;
    }

    auto &cases = node->data.switch_expr.cases;
    if (cases.len == 0) {
        return false;
    }

    for (auto scase : cases) {
        if (!scase || scase->type != NodeType::CaseExpr ||
            !block_returns_value(scase->data.case_expr.body)) {
            return false;
        }
    }

    return true;
}

Node *Parser::parse_stmt(bool *as_expr) {
    auto token = get();
    switch (token->type) {
    case TokenType::KW_IF: {
        auto node = parse_if_expr(false);
        // If-expression: only when every branch is syntactically value-producing.
        if (next_is(TokenType::RBRACE) && if_expr_returns_value(node))
            *as_expr = true;
        return node;
    }

    case TokenType::KW_SWITCH: {
        auto node = parse_switch_expr(false);
        if (next_is(TokenType::SEMICOLON)) {
            consume();
        } else if (next_is(TokenType::RBRACE) && switch_expr_returns_value(node)) {
            // Last expression in block — treat as block result
            *as_expr = true;
        }
        // Otherwise: no semicolon needed (statement form)
        return node;
    }

    case TokenType::KW_FOR:
        return parse_for_stmt();

    case TokenType::KW_WHILE:
        return parse_while_stmt();

    case TokenType::KW_TRY: {
        auto expr = parse_expr();
        if (expr->type == NodeType::TryExpr && expr->data.try_expr.catch_block) {
            // try ... catch { ... } — semicolon optional (statement form)
            if (next_is(TokenType::SEMICOLON)) {
                consume();
            } else if (next_is(TokenType::RBRACE)) {
                *as_expr = true;
            }
            // Otherwise: no semicolon needed (statement form)
        } else {
            // try f() catch ErrorType — expression form, needs semicolon
            if (next_is(TokenType::SEMICOLON)) {
                consume();
            } else {
                *as_expr = true;
            }
        }
        return expr;
    }

    case TokenType::KW_VAR:
    case TokenType::KW_LET:
    case TokenType::KW_THIS:
    case TokenType::KW_NEW:
    case TokenType::KW_DELETE:
    case TokenType::IDEN:
    case TokenType::BOOL:
    case TokenType::NULLP:
    case TokenType::INT:
    case TokenType::FLOAT:
    case TokenType::CHAR:
    case TokenType::STRING:
    case TokenType::C_STRING:
    case TokenType::LPAREN:
    case TokenType::LBRACK:
    case TokenType::ADD:
    case TokenType::SUB:
    case TokenType::MUL:
    case TokenType::AND:
    case TokenType::XOR:
    case TokenType::NOT:
    case TokenType::INC:
    case TokenType::DEC:
    case TokenType::KW_AWAIT: {
        if (next_is(TokenType::KW_VAR) || next_is(TokenType::KW_LET)) {
            return parse_var_decl(false);
        }

        Node *expr;
        if (is_construct_expr_with_type()) {
            expr = parse_construct_expr();
        } else {
            expr = parse_expr();
        }
        if (next_is(TokenType::SEMICOLON)) {
            consume();
        } else {
            *as_expr = true;
        }
        return expr;
    }

    case TokenType::KW_RETURN:
        return parse_return_stmt();

    case TokenType::KW_THROW: {
        auto token = expect(TokenType::KW_THROW);
        auto node = create_node(NodeType::ThrowStmt, token);
        node->data.throw_stmt.expr = parse_child_expr_construct(false, node);
        expect(TokenType::SEMICOLON);
        return node;
    }

    case TokenType::KW_CONTINUE:
    case TokenType::KW_BREAK:
        return parse_branch_stmt();

    case TokenType::LBRACE:
        return parse_block();

    case TokenType::KW_UNSAFE: {
        consume();
        auto block = parse_block();
        block->data.block.is_unsafe = true;
        return block;
    }

    case TokenType::SEMICOLON:
        consume();
        return create_node(NodeType::EmptyStmt, token);

    default:
        unexpected(token);
        recover_to_statement_boundary();
        return create_error_node();
    }
}

bool Parser::next_is(TokenType token_type) { return get()->type == token_type; }

Node *Parser::parse_expr() {
    auto lhs = parse_expr_clause(true);
    auto token = get();
    switch (token->type) {
    case TokenType::ASS:
    case TokenType::ADD_ASS:
    case TokenType::SUB_ASS:
    case TokenType::MUL_ASS:
    case TokenType::DIV_ASS:
    case TokenType::MOD_ASS:
    case TokenType::AND_ASS:
    case TokenType::OR_ASS:
    case TokenType::XOR_ASS:
    case TokenType::LSHIFT_ASS:
    case TokenType::RSHIFT_ASS:
        consume();
        break;

    default:
        return lhs;
    }
    auto node = create_node(NodeType::BinOpExpr, token);
    node->data.bin_op_expr.op1 = lhs;
    node->data.bin_op_expr.op_type = token->type;
    node->data.bin_op_expr.op2 = parse_child_expr_construct(false, node);
    return node;
}

Node *Parser::parse_expr_clause(bool lhs) { return parse_binary_expr(lhs, nullptr, DEFAULT_PREC); }

Node *Parser::parse_child_expr_construct(bool lhs, Node *parent) {
    // Try to parse as type construct expression if that's possible (e.g., Array<int>{1, 2, 3})
    if (!lhs && is_construct_expr_with_type()) {
        return parse_postfix_expr(parse_construct_expr(), lhs, parent);
    }

    // Fall back to normal expression parsing
    return parse_child_expr(lhs, parent);
}

static Node *parse_binary_operand(Parser *parser, bool lhs, Node *parent) {
    // Binary expression parsing normally starts from unary precedence, but a typed
    // construct like `Point{}` is also a valid operand and may still be followed
    // by postfix operators like `.field` or `.method()`.
    if (!lhs && parser->is_construct_expr_with_type()) {
        return parser->parse_postfix_expr(parser->parse_construct_expr(), lhs, parent);
    }
    return parser->parse_unary_expr(lhs, parent);
}

Node *Parser::parse_binary_expr(bool lhs, Node *parent, int prec) {
    auto op1 = parse_binary_operand(this, lhs, parent);
    for (;;) {
        auto op_token = get();
        if (op_token->type == TokenType::END) {
            error(op_token, errors::UNEXPECTED_EOF);
            return op1;
        }
        auto tok_type = op_token->type;
        if (op_token->type == TokenType::GT &&
            lookahead(1)->type == TokenType::GT) { // check RSHIFT >> operator
            consume();
            tok_type = TokenType::RSHIFT;
        }
        // ?? null coalesce: two QUES tokens → treat as single operator at LOR precedence
        if (op_token->type == TokenType::QUES && lookahead(1)->type == TokenType::QUES) {
            consume(); // consume first ?
            consume(); // consume second ?
            auto op2 = parse_binary_expr(lhs, parent, 3); // precedence 2 + 1
            auto node = create_node(NodeType::BinOpExpr, op_token);
            node->data.bin_op_expr.op1 = op1;
            node->data.bin_op_expr.op_type = TokenType::QUES;
            node->data.bin_op_expr.op2 = op2;
            op1 = node;
            continue;
        }
        auto op_prec = get_op_precedence(tok_type);
        if (op_prec < prec) {
            return op1;
        }
        consume();

        // Special handling for 'as' cast operator - RHS is a type expression, not value expression
        if (tok_type == TokenType::KW_AS) {
            auto node = create_node(NodeType::CastExpr, op_token);
            node->data.cast_expr.expr = op1;
            node->data.cast_expr.dest_type = parse_type_expr(true);
            op1 = node;
        } else {
            auto op2 = parse_binary_expr(lhs, parent, op_prec + 1);
            auto node = create_node(NodeType::BinOpExpr, op_token);
            node->data.bin_op_expr.op1 = op1;
            node->data.bin_op_expr.op_type = tok_type;
            node->data.bin_op_expr.op2 = op2;
            op1 = node;
        }
    }
    return op1;
}

Node *Parser::parse_child_expr(bool lhs, Node *parent) {
    return parse_binary_expr(lhs, parent, DEFAULT_PREC);
}

Node *Parser::parse_postfix_expr(Node *node, bool lhs, Node *parent) {
    for (;;) {
        auto token = get();
        switch (token->type) {
        case TokenType::DOT:
            if (lookahead(1)->type == TokenType::LPAREN) {
                node = parse_type_info_expr(node);
                break;
            }
            node = parse_dot_expr(node);
            break;

        case TokenType::QUES_DOT:
            node = parse_dot_expr(node, true);
            break;

        case TokenType::LBRACK:
            node = parse_index_expr(node);
            break;

        case TokenType::LT:
            // Check if this is a function call with type parameters
            if (is_function_call_with_type_params()) {
                node = parse_fn_call_with_type_params(node, lhs, parent);
            } else if (is_type_access_with_type_params()) {
                // Type<Args>.method() — parse as SubtypeExpr, loop continues with '.'
                auto subtype_node = create_node(NodeType::SubtypeExpr, node->token);
                subtype_node->data.subtype_expr.type = node;
                expect(TokenType::LT);
                for (;;) {
                    auto tok = get();
                    if (tok->type == TokenType::END) {
                        error(tok, errors::UNEXPECTED_EOF);
                        return subtype_node;
                    }
                    if (tok->type == TokenType::GT) {
                        break;
                    }
                    // Check for ...T spread in type args
                    if (tok->type == TokenType::ELLIPSIS) {
                        consume(); // skip past ...
                        auto param = parse_type_expr(true);
                        auto expansion = create_node(NodeType::PackExpansion, tok);
                        expansion->data.pack_expansion.expr = param;
                        subtype_node->data.subtype_expr.args.add(expansion);
                    } else {
                        auto param = parse_type_expr(true);
                        subtype_node->data.subtype_expr.args.add(param);
                    }
                    if (!at_comma(TokenType::GT)) {
                        break;
                    }
                    consume();
                }
                expect(TokenType::GT);
                node = subtype_node;
            } else {
                return node; // Let the expression parser handle it as a comparison
            }
            break;

        case TokenType::LPAREN:
            node = parse_fn_call_expr(node, lhs, parent);
            break;

        case TokenType::INC:
        case TokenType::DEC:
        case TokenType::LNOT: {
            auto op1 = node;
            node = create_unary_expr_node(read());
            node->data.unary_op_expr.op1 = op1;
            node->data.unary_op_expr.is_suffix = true;
            break;
        }

        default:
            return node;
        }
    }
}

Node *Parser::parse_unary_expr(bool lhs, Node *parent) {
    auto token = get();
    switch (token->type) {
    case TokenType::KW_NEW:
        return parse_construct_expr();

    case TokenType::KW_SIZEOF:
    case TokenType::KW_DELETE:
        return parse_prefix_expr();

    case TokenType::KW_MOVE: {
        consume();
        auto node = create_unary_expr_node(token);
        node->data.unary_op_expr.op_type = TokenType::KW_MOVE;
        node->data.unary_op_expr.op1 = parse_binary_expr(lhs, parent, UNARY_PREC);
        return node;
    }

    case TokenType::ADD:
    case TokenType::AND:
    case TokenType::MUL:
    case TokenType::SUB:
    case TokenType::NOT:
    case TokenType::LNOT:
    case TokenType::INC:
    case TokenType::DEC: {
        consume();
        auto node = create_unary_expr_node(token);

        if (token->type == TokenType::AND && get()->type == TokenType::KW_MUT) {
            consume();
            node->data.unary_op_expr.op_type = TokenType::MUTREF;
        } else if (token->type == TokenType::AND && get()->type == TokenType::KW_MUTEX) {
            consume();
            node->data.unary_op_expr.op_type = TokenType::MUTEXREF;
        } else if (token->type == TokenType::AND && get()->type == TokenType::KW_MOVE) {
            consume();
            node->data.unary_op_expr.op_type = TokenType::MOVEREF;
        }

        auto operand = parse_binary_expr(lhs, parent, UNARY_PREC);
        if (operand && operand->type == NodeType::Error) {
            // If operand parsing failed, return error node instead of malformed unary expr
            return operand;
        }
        node->data.unary_op_expr.op1 = operand;
        return node;
    }

    case TokenType::KW_TRY: {
        consume();
        auto node = create_node(NodeType::TryExpr, token);
        node->data.try_expr.expr = parse_binary_expr(lhs, parent, UNARY_PREC);
        if (next_is(TokenType::KW_CATCH)) {
            consume();
            if (next_is(TokenType::LBRACE)) {
                // catch { ... } — catch-all
                node->data.try_expr.catch_block = parse_block();
            } else {
                // catch Type — parse the error type
                node->data.try_expr.catch_expr = parse_primary_expr(false, node);
                if (next_is(TokenType::KW_AS)) {
                    // catch MyError as err { ... } — typed catch with binding
                    consume();
                    auto name_token = expect(TokenType::IDEN);
                    auto var = create_node(NodeType::VarDecl, name_token);
                    var->name = name_token->str;
                    node->data.try_expr.catch_err_var = var;
                    auto catch_scope = m_ctx->resolver->push_scope(node);
                    m_ctx->resolver->declare_symbol(var->name, var);
                    node->data.try_expr.catch_block = parse_block();
                    m_ctx->resolver->pop_scope();
                } else if (next_is(TokenType::LBRACE)) {
                    // catch MyError { ... } — typed catch without binding
                    node->data.try_expr.catch_block = parse_block();
                }
                // else: catch MyError — Result mode, no block
            }
        }
        return node;
    }

    case TokenType::KW_AWAIT: {
        consume();
        auto node = create_node(NodeType::AwaitExpr, token);
        node->data.await_expr.expr = parse_binary_expr(lhs, parent, UNARY_PREC);
        return node;
    }

    default:
        return parse_primary_expr(lhs, parent);
    }
}

Node *Parser::parse_primary_expr(bool lhs, Node *parent) {
    return parse_postfix_expr(parse_operand(lhs, parent), lhs, parent);
}

Node *Parser::parse_operand(bool lhs, Node *parent) {
    auto token = get();
    switch (token->type) {
    case TokenType::KW_THIS: {
        consume();
        auto node = create_node(NodeType::Identifier, token);
        node->data.identifier.kind = IdentifierKind::This;
        node->name = "this";
        return node;
    }
    case TokenType::KW_THIS_TYPE:
    case TokenType::KW_NEW:
    case TokenType::KW_DELETE:
    case TokenType::IDEN: {
        return parse_identifier();
    }
    case TokenType::INT:
    case TokenType::BOOL:
    case TokenType::NULLP:
    case TokenType::KW_UNDEFINED:
    case TokenType::KW_ZEROINIT:
    case TokenType::FLOAT:
    case TokenType::CHAR:
    case TokenType::STRING:
    case TokenType::C_STRING: {
        consume();
        return create_node(NodeType::LiteralExpr, token);
    }
    case TokenType::LPAREN: {
        if (is_arrow_lambda_ahead()) {
            return parse_fn_lambda();
        }
        if (lookahead(1)->type == TokenType::RPAREN) {
            // () — unit value
            consume(); // (
            auto node = create_node(NodeType::UnitExpr, token);
            consume(); // )
            return node;
        }
        consume();
        auto first = parse_child_expr(lhs, parent);
        if (get()->type == TokenType::COMMA) {
            // (a, b, ...) — tuple expression
            auto node = create_node(NodeType::TupleExpr, token);
            node->data.tuple_expr.items.add(first);
            while (get()->type == TokenType::COMMA) {
                consume(); // ,
                if (get()->type == TokenType::RPAREN) break; // trailing comma
                node->data.tuple_expr.items.add(parse_child_expr(lhs, parent));
            }
            expect(TokenType::RPAREN);
            return node;
        }
        // (a) — parenthesized expression
        auto node = create_node(NodeType::ParenExpr, token);
        node->data.child_expr = first;
        expect(TokenType::RPAREN);
        return node;
    }
    case TokenType::KW_FUNC: {
        return parse_fn_lambda();
    }
    case TokenType::KW_IF: {
        return parse_if_expr(true);
    }
    case TokenType::KW_SWITCH: {
        return parse_switch_expr(true);
    }
    case TokenType::LBRACE: {
        return parse_construct_expr();
    }
    case TokenType::LBRACK: {
        // [N]Type{...} fixed-size array construct
        if (lookahead(1)->type == TokenType::INT && lookahead(2)->type == TokenType::RBRACK) {
            auto type_node = parse_type_expr(true);
            auto node = create_node(NodeType::ConstructExpr, token);
            node->data.construct_expr.type = type_node;
            expect(TokenType::LBRACE);
            for (;;) {
                auto tok = get();
                if (tok->type == TokenType::END) {
                    error(tok, errors::UNEXPECTED_EOF);
                    return node;
                }
                if (tok->type == TokenType::RBRACE) {
                    break;
                }
                node->data.construct_expr.items.add(parse_child_expr_construct(false, node));
                if (!at_comma(TokenType::RBRACE))
                    break;
                consume();
            }
            expect(TokenType::RBRACE);
            return node;
        }
        // Array literal: [expr, expr, ...]
        consume();
        auto node = create_node(NodeType::ConstructExpr, token);
        node->data.construct_expr.is_array_literal = true;
        for (;;) {
            auto tok = get();
            if (tok->type == TokenType::END) {
                error(tok, errors::UNEXPECTED_EOF);
                return node;
            }
            if (tok->type == TokenType::RBRACK) {
                break;
            }
            node->data.construct_expr.items.add(parse_child_expr_construct(false, node));
            if (!at_comma(TokenType::RBRACK))
                break;
            consume();
        }
        expect(TokenType::RBRACK);
        return node;
    }
    default:
        unexpected(token);
    }
    return create_error_node();
}

Node *Parser::parse_fn_call_expr(Node *fn_expr, bool lhs, Node *parent) {
    auto node = create_node(NodeType::FnCallExpr, fn_expr->token);
    node->data.fn_call_expr.fn_ref_expr = fn_expr;
    assert(fn_expr && node->data.fn_call_expr.args.len == 0);
    expect(TokenType::LPAREN);

    for (;;) {
        auto tok = get();
        if (tok->type == TokenType::END) {
            error(tok, errors::UNEXPECTED_EOF);
            return node;
        }
        if (tok->type == TokenType::RPAREN) {
            break;
        } else {
            // Check for pack expansion: ...expr
            if (next_is(TokenType::ELLIPSIS)) {
                auto ellipsis = get();
                consume();
                // Inside parens, '{' is unambiguous — always allow construct exprs
                auto arg = parse_child_expr_construct(false, parent);
                auto expansion = create_node(NodeType::PackExpansion, ellipsis);
                expansion->data.pack_expansion.expr = arg;
                node->data.fn_call_expr.args.add(expansion);
            } else {
                // Inside parens, '{' is unambiguous — always allow construct exprs
                auto arg = parse_child_expr_construct(false, parent);
                node->data.fn_call_expr.args.add(arg);
            }
        }
        if (!at_comma(TokenType::RPAREN)) {
            break;
        }
        consume();
    }
    node->end_token = expect(TokenType::RPAREN);
    return node;
}

bool Parser::is_function_call_with_type_params() {
    // Use proper type expression parsing to distinguish func<type>() from a < b
    int pos = 1; // Start after the '<' token

    // Parse type arguments
    for (;;) {
        auto token = lookahead(pos);
        if (token->type == TokenType::END) {
            return false;
        }
        if (token->type == TokenType::GT) {
            pos++;
            break;
        }

        // Try to parse a type expression at this position
        if (!try_parse_type_expr_lookahead(pos)) {
            return false;
        }

        token = lookahead(pos);
        if (token->type == TokenType::COMMA) {
            pos++;
        } else if (token->type == TokenType::GT) {
            pos++;
            break;
        } else {
            return false;
        }
    }

    // After parsing type arguments, next token should be '('
    auto token = lookahead(pos);
    return token->type == TokenType::LPAREN;
}

bool Parser::is_type_access_with_type_params() {
    // Check if we have Type<Args>.method — type params followed by '.'
    int pos = 1; // Start after the '<' token

    for (;;) {
        auto token = lookahead(pos);
        if (token->type == TokenType::END) {
            return false;
        }
        if (token->type == TokenType::GT) {
            pos++;
            break;
        }

        if (!try_parse_type_expr_lookahead(pos)) {
            return false;
        }

        token = lookahead(pos);
        if (token->type == TokenType::COMMA) {
            pos++;
        } else if (token->type == TokenType::GT) {
            pos++;
            break;
        } else {
            return false;
        }
    }

    auto token = lookahead(pos);
    return token->type == TokenType::DOT;
}

bool Parser::is_construct_expr_with_type() {
    // Check if we have a type expression followed by '{'
    // This handles cases like Array<int>{1, 2, 3}
    int pos = 0;

    // Try to parse the complete type expression (handles both simple and generic types)
    if (!try_parse_type_expr_lookahead(pos, true)) {
        return false;
    }

    // Check if after the type expression we have a '{'
    auto token = lookahead(pos);
    return token->type == TokenType::LBRACE;
}

Node *Parser::parse_fn_call_with_type_params(Node *fn_expr, bool lhs, Node *parent) {
    auto node = create_node(NodeType::FnCallExpr, fn_expr->token);
    node->data.fn_call_expr.fn_ref_expr = fn_expr;

    // Parse type parameters <T, U>
    expect(TokenType::LT);
    for (;;) {
        auto token = get();
        if (token->type == TokenType::END) {
            error(token, errors::UNEXPECTED_EOF);
            return node;
        }
        if (token->type == TokenType::GT) {
            break;
        }

        auto type_arg = parse_type_expr(true);
        node->data.fn_call_expr.type_args.add(type_arg);

        if (!at_comma(TokenType::GT)) {
            break;
        }
        consume();
    }
    expect(TokenType::GT);

    // Parse function arguments
    expect(TokenType::LPAREN);
    for (;;) {
        auto tok = get();
        if (tok->type == TokenType::END) {
            error(tok, errors::UNEXPECTED_EOF);
            return node;
        }
        if (tok->type == TokenType::RPAREN) {
            break;
        } else {
            auto arg = parse_child_expr(lhs, parent);
            node->data.fn_call_expr.args.add(arg);
        }
        if (!at_comma(TokenType::RPAREN)) {
            break;
        }
        consume();
    }
    node->end_token = expect(TokenType::RPAREN);
    return node;
}

bool Parser::try_parse_fn_type_lookahead(int &pos) {
    // Skip func<'static> lifetime params
    auto token = lookahead(pos);
    if (token->type == TokenType::LT) {
        pos++;
        while (true) {
            token = lookahead(pos);
            if (token->type == TokenType::GT || token->type == TokenType::END)
                break;
            pos++;
        }
        if (token->type == TokenType::GT)
            pos++;
    }

    // func might not have parentheses
    token = lookahead(pos);
    if (token->type != TokenType::LPAREN) {
        return true;
    }
    pos++;

    // Parse parameters
    for (;;) {
        token = lookahead(pos);
        if (token->type == TokenType::END) {
            return false;
        }
        if (token->type == TokenType::RPAREN) {
            pos++;
            break;
        }

        // Handle variadic parameters
        if (token->type == TokenType::ELLIPSIS) {
            pos++;
            token = lookahead(pos);
        }

        // Parse parameter name (required in Chi)
        if (token->type != TokenType::IDEN) {
            return false;
        }
        pos++; // Skip parameter name

        // Expect colon
        token = lookahead(pos);
        if (token->type != TokenType::COLON) {
            return false;
        }
        pos++; // Skip colon

        // Parse parameter type
        if (!try_parse_type_expr_lookahead(pos)) {
            return false;
        }

        token = lookahead(pos);
        if (token->type == TokenType::COMMA) {
            pos++;
        } else if (token->type == TokenType::RPAREN) {
            pos++;
            break;
        } else {
            return false;
        }
    }

    // Check if there's a return type
    token = lookahead(pos);
    if (token->type != TokenType::RPAREN && token->type != TokenType::SEMICOLON &&
        token->type != TokenType::COMMA && token->type != TokenType::GT) {
        // Parse return type
        if (!try_parse_type_expr_lookahead(pos)) {
            return false;
        }
    }

    return true;
}

bool Parser::try_parse_type_expr_lookahead(int &pos, bool struct_only) {
    // Parse sigils (*, &, etc.) first
    auto token = lookahead(pos);
    if (token->type == TokenType::END) {
        return false;
    }

    // Handle [N]T fixed-size array type
    if (token->type == TokenType::LBRACK) {
        pos++;
        if (lookahead(pos)->type != TokenType::INT)
            return false;
        pos++;
        if (lookahead(pos)->type != TokenType::RBRACK)
            return false;
        pos++;
        return try_parse_type_expr_lookahead(pos, struct_only);
    }

    // Handle pointer sigil
    if (token->type == TokenType::MUL) {
        pos++;
        token = lookahead(pos);
        if (token->type == TokenType::END) {
            return false;
        }
    }

    // Handle reference sigil: &T, &mut T, &mutex T, &move T, &'a T, &(mut, 'a) T, &[T]
    if (token->type == TokenType::AND) {
        auto span_sigil_kind = SigilKind::Reference;
        pos++;
        if (!try_parse_reference_type_prefix_lookahead(pos, span_sigil_kind)) {
            return false;
        }
        token = lookahead(pos);
        if ((span_sigil_kind == SigilKind::Reference || span_sigil_kind == SigilKind::MutRef) &&
            token->type == TokenType::LBRACK && lookahead(pos + 1)->type != TokenType::INT) {
            pos++;
            if (!try_parse_type_expr_lookahead(pos, struct_only))
                return false;
            if (lookahead(pos)->type != TokenType::RBRACK)
                return false;
            pos++;
            return true;
        }
    }

    if (!struct_only) {
        // Handle function types (func(...) ...)
        if (token->type == TokenType::KW_FUNC) {
            pos++;
            return try_parse_fn_type_lookahead(pos);
        }
    }

    // Must be an identifier-based type
    if (token->type != TokenType::IDEN) {
        return false;
    }
    pos++;

    // Handle dot-qualified types (e.g., mod.Greeting, a.b.c)
    while (lookahead(pos)->type == TokenType::DOT) {
        pos++;
        if (lookahead(pos)->type != TokenType::IDEN) {
            return false;
        }
        pos++;
    }

    // Handle generic types (e.g., Container<T>)
    token = lookahead(pos);
    if (token->type == TokenType::LT) {
        pos++;

        // Parse type arguments
        for (;;) {
            token = lookahead(pos);
            if (token->type == TokenType::END) {
                return false;
            }
            if (token->type == TokenType::GT) {
                pos++;
                break;
            }

            // Handle lifetime args: Type<'a, 'static>
            if (token->type == TokenType::LIFETIME) {
                pos++;
                // Skip optional bound: 'a: 'b
                if (lookahead(pos)->type == TokenType::COLON &&
                    lookahead(pos + 1)->type == TokenType::LIFETIME) {
                    pos += 2;
                }
            }
            // Recursively parse type argument
            else if (!try_parse_type_expr_lookahead(pos)) {
                return false;
            }

            token = lookahead(pos);
            if (token->type == TokenType::COMMA) {
                pos++;
            } else if (token->type == TokenType::GT) {
                pos++;
                break;
            } else {
                return false;
            }
        }
    }

    // Handle dot-qualified access after generic args (e.g., Container<int>.Single)
    while (lookahead(pos)->type == TokenType::DOT) {
        pos++;
        if (lookahead(pos)->type != TokenType::IDEN) {
            return false;
        }
        pos++;
    }

    return true;
}

Node *Parser::parse_return_stmt() {
    auto token = expect(TokenType::KW_RETURN);
    auto node = create_node(NodeType::ReturnStmt, token);
    if (get()->type != TokenType::SEMICOLON) {
        node->data.return_stmt.expr = parse_child_expr_construct(false, node);
    }
    expect(TokenType::SEMICOLON);
    return node;
}

Node *Parser::parse_branch_stmt() {
    auto node = create_node(NodeType::BranchStmt, read());
    expect(TokenType::SEMICOLON);
    return node;
}

void Parser::add_to_scope(Node *node) {
    if (node->name.empty()) {
        // Don't add nodes without names to scope - this can happen with malformed code
        return;
    }
    add_to_scope(node, node->name);
}

void Parser::add_to_scope(Node *node, const string &name) {
    auto ok = m_ctx->resolver->declare_symbol(name, node);
    if (!ok && !m_ctx->format_mode) {
        error(node->token, errors::REDECLARED, name);
    }
}

Node *Parser::parse_if_expr(bool require_value) {
    auto kw = expect(TokenType::KW_IF);
    auto node = create_node(NodeType::IfExpr, kw);
    auto scope = m_ctx->resolver->push_scope(node);
    if (next_is(TokenType::KW_LET) || next_is(TokenType::KW_VAR)) {
        auto bind_kw = read();
        auto kind = bind_kw->type == TokenType::KW_VAR ? VarKind::Mutable : VarKind::Immutable;
        if (starts_destructure_pattern()) {
            node->data.if_expr.binding_decl = parse_any_destructure_pattern(kind);
        } else if (looks_like_case_pattern_clause()) {
            node->data.if_expr.binding_clause = parse_type_expr(true);
            node->data.if_expr.binding_decl = parse_any_destructure_pattern(kind);
        } else {
            auto iden = expect(TokenType::IDEN);
            auto binding = create_node(NodeType::VarDecl, iden);
            binding->data.var_decl.identifier = iden;
            binding->data.var_decl.kind = kind;
            binding->name = iden->get_name();
            node->data.if_expr.binding_decl = binding;
        }
        expect(TokenType::ASS);
    }
    node->data.if_expr.condition = parse_expr();
    if (node->data.if_expr.binding_decl) {
        if (node->data.if_expr.binding_decl->type == NodeType::DestructureDecl) {
            register_destructure_bindings(this, node->data.if_expr.binding_decl);
        } else {
            add_to_scope(node->data.if_expr.binding_decl);
        }
    }
    // Then block: { ... } or => expr
    Token *then_arrow = next_is(TokenType::ARROW) ? read() : nullptr;
    node->data.if_expr.then_block = parse_block(scope, then_arrow);
    auto token = get();
    if (token->type == TokenType::KW_ELSE) {
        consume();
        token = get();
        if (token->type == TokenType::KW_IF) {
            node->data.if_expr.else_node = parse_if_expr(require_value);
        } else if (token->type == TokenType::ARROW) {
            node->data.if_expr.else_node = parse_block(nullptr, read());
        } else if (token->type == TokenType::LBRACE) {
            node->data.if_expr.else_node = parse_block();
        } else {
            error(token, "expecting if statement or block");
        }
    }
    m_ctx->resolver->pop_scope();
    if (require_value && !if_expr_returns_value(node)) {
        error(node->token,
              "if used as an expression must have an else and every branch must produce a value");
    }
    return node;
}

Node *Parser::parse_for_stmt() {
    auto kw = expect(TokenType::KW_FOR);
    auto node = create_node(NodeType::ForStmt, kw);
    auto scope = m_ctx->resolver->push_scope(node);
    if (!next_is(TokenType::LBRACE)) {
        Node *expr;
        ForLoopKind kind = ForLoopKind::Empty;
        if (next_is(TokenType::KW_VAR) || next_is(TokenType::KW_LET)) {
            expr = parse_var_decl(false);
            kind = ForLoopKind::Ternary;
        } else {
            bool is_range = false;
            if (next_is(TokenType::AND)) {
                node->data.for_stmt.bind_sigil = SigilKind::Reference;
                consume();

                if (next_is(TokenType::KW_MUT)) {
                    consume();
                    node->data.for_stmt.bind_sigil = SigilKind::MutRef;
                } else if (next_is(TokenType::KW_MUTEX)) {
                    consume();
                    node->data.for_stmt.bind_sigil = SigilKind::MutexRef;
                }

                is_range = true;
            }

            // Check the 'in' keyword for for in loop, or fallback to ternary
            if (is_range || lookahead(1)->type == TokenType::KW_IN ||
                lookahead(1)->type == TokenType::COMMA) {
                auto iden = expect(TokenType::IDEN);
                auto bind = create_node(NodeType::BindIdentifier, iden);
                bind->parent_fn = get_scope()->find_parent(NodeType::FnDef);
                node->data.for_stmt.bind = bind;
                kind = ForLoopKind::Range;
                if (next_is(TokenType::COMMA)) {
                    consume();
                    auto index_iden = expect(TokenType::IDEN);
                    auto index_bind = create_node(NodeType::BindIdentifier, index_iden);
                    index_bind->parent_fn = get_scope()->find_parent(NodeType::FnDef);
                    node->data.for_stmt.index_bind = index_bind;
                }
                expect(TokenType::KW_IN);
            } else {
                expr = parse_expr();
                if (next_is(TokenType::SEMICOLON)) {
                    consume();
                    kind = ForLoopKind::Ternary;
                } else {
                    unexpected(get());
                }
            }
        }

        node->data.for_stmt.kind = kind;
        if (kind == ForLoopKind::Ternary) {
            node->data.for_stmt.init = expr;
            if (!next_is(TokenType::SEMICOLON)) {
                node->data.for_stmt.condition = parse_expr();
            }
            expect(TokenType::SEMICOLON);
            if (!next_is(TokenType::LBRACE)) {
                node->data.for_stmt.post = parse_expr();
            }
        }
        if (kind == ForLoopKind::Range) {
            auto expr = parse_expr();
            if (next_is(TokenType::DOT_DOT)) {
                consume();
                auto range = create_node(NodeType::RangeExpr, expr->token);
                range->data.range_expr.start = expr;
                range->data.range_expr.end = parse_expr();
                node->data.for_stmt.expr = range;
            } else {
                node->data.for_stmt.expr = expr;
            }
            auto bind = node->data.for_stmt.bind;
            if (bind->name != "_") {
                add_to_scope(bind, bind->name);
            }
            auto index_bind = node->data.for_stmt.index_bind;
            if (index_bind && index_bind->name != "_") {
                add_to_scope(index_bind, index_bind->name);
            }
        }
    }
    node->data.for_stmt.body = parse_block(scope);
    m_ctx->resolver->pop_scope();
    return node;
}

Node *Parser::parse_while_stmt() {
    auto kw = expect(TokenType::KW_WHILE);
    auto node = create_node(NodeType::WhileStmt, kw);
    auto scope = m_ctx->resolver->push_scope(node);
    if (!next_is(TokenType::LBRACE)) {
        auto expr = parse_expr();
        node->data.while_stmt.condition = expr;
    }
    node->data.while_stmt.body = parse_block(scope);
    m_ctx->resolver->pop_scope();
    return node;
}

void Parser::skip_block() {
    expect(TokenType::LBRACE);
    long block_level = 1;
    while (block_level > 0) {
        auto tok = read();
        if (tok->type == TokenType::END) {
            error(tok, errors::UNEXPECTED_EOF);
            return;
        }
        if (tok->type == TokenType::LBRACE) {
            block_level++;
        } else if (tok->type == TokenType::RBRACE) {
            block_level--;
        }
    }
}

ContainerKind Parser::get_container_kind(TokenType keyword) {
    switch (keyword) {
    case TokenType::KW_ENUM:
        return ContainerKind::Enum;
    case TokenType::KW_UNION:
        return ContainerKind::Union;
    case TokenType::KW_INTERFACE:
        return ContainerKind::Interface;
    default:
        return ContainerKind::Struct;
    }
}

Node *Parser::create_struct_node(Token *keyword, const string &name) {
    auto node = create_node(NodeType::StructDecl, keyword);
    node->data.struct_decl.kind = get_container_kind(keyword->type);
    node->name = name;
    return node;
}

Node *Parser::parse_enum_decl(DeclSpec *decl_spec) {
    decl_spec = parse_decl_spec(decl_spec);
    auto kw = expect(TokenType::KW_ENUM);
    auto iden = expect(TokenType::IDEN);
    Node *node = create_node(NodeType::EnumDecl, iden);
    node->start_token = kw;
    node->data.enum_decl.decl_spec = decl_spec;

    auto &params = node->data.enum_decl.type_params;
    if (next_is(TokenType::LT)) {
        expect(TokenType::LT);
        Token *token;
        for (;;) {
            token = get();
            if (token->type == TokenType::END) {
                error(token, errors::UNEXPECTED_EOF);
                return node;
            }
            if (token->type == TokenType::GT) {
                break;
            }
            bool is_variadic = token->type == TokenType::ELLIPSIS;
            if (is_variadic) consume(); // skip past ...
            auto param_iden = expect(TokenType::IDEN);
            auto param_node = create_node(NodeType::TypeParam, param_iden);
            param_node->data.type_param.is_variadic = is_variadic;

            // Check for colon syntax for type bounds: T: Trait1 + Trait2 + ...
            if (next_is(TokenType::COLON)) {
                consume(); // consume the colon
                do {
                    param_node->data.type_param.type_bounds.add(parse_type_expr(true));
                } while (next_is(TokenType::ADD) && (consume(), true));
            }

            // Check for = syntax for default type: T = int
            if (next_is(TokenType::ASS)) {
                consume(); // consume the =
                param_node->data.type_param.default_type = parse_type_expr(true);
            }

            param_node->data.type_param.index = params.len;
            param_node->data.type_param.source_decl = node;
            params.add(param_node);

            if (!at_comma(TokenType::GT)) {
                break;
            }
            consume();
        }
        expect(TokenType::GT);
    }

    if (next_is(TokenType::COLON)) {
        consume();
        node->data.enum_decl.discriminator_type = parse_type_expr(true);
        if (next_is(TokenType::KW_AS)) {
            consume();
            node->data.enum_decl.discriminator_field = expect(TokenType::IDEN);
        }
    }

    save_block_pos(node);
    skip_block();

    if (next_is(TokenType::SEMICOLON)) {
        consume();
    }

    node->end_token = lookahead(-1);
    add_to_scope(node);
    return node;
}

Node *Parser::parse_struct_decl(TokenType keyword, DeclSpec *decl_spec) {
    decl_spec = parse_decl_spec(decl_spec);
    auto kw = expect(keyword);
    auto iden = expect(TokenType::IDEN);
    Node *node = create_struct_node(kw, iden->str);
    iden->semantic_node = node;
    node->start_token = kw;
    node->data.struct_decl.decl_spec = decl_spec;
    auto &params = node->data.struct_decl.type_params;
    if (next_is(TokenType::LT)) {
        expect(TokenType::LT);
        Token *token;
        for (;;) {
            token = get();
            if (token->type == TokenType::END) {
                error(token, errors::UNEXPECTED_EOF);
                return node;
            }
            if (token->type == TokenType::GT) {
                break;
            }
            // Lifetime param: 'a or 'a: 'b
            if (token->type == TokenType::LIFETIME) {
                node->data.struct_decl.lifetime_params.add(parse_lifetime_param(token));
            } else {
                // Type param
                bool is_variadic = token->type == TokenType::ELLIPSIS;
                if (is_variadic) consume(); // skip past ...
                auto param_iden = expect(TokenType::IDEN);
                auto param_node = create_node(NodeType::TypeParam, param_iden);
                param_node->data.type_param.is_variadic = is_variadic;

                // Check for colon syntax for type bounds: T: Trait1 + Trait2 + ...
                if (next_is(TokenType::COLON)) {
                    consume(); // consume the colon
                    // Check if first bound is a lifetime: T: 'a
                    if (next_is(TokenType::LIFETIME)) {
                        param_node->data.type_param.lifetime_bound = get()->str;
                        consume();
                    } else {
                        do {
                            param_node->data.type_param.type_bounds.add(parse_type_expr(true));
                        } while (next_is(TokenType::ADD) && (consume(), true));
                    }
                }

                // Check for = syntax for default type: T = int
                if (next_is(TokenType::ASS)) {
                    consume(); // consume the =
                    param_node->data.type_param.default_type = parse_type_expr(true);
                }

                param_node->data.type_param.index = params.len;
                param_node->data.type_param.source_decl = node;
                params.add(param_node);
            }

            if (!at_comma(TokenType::GT)) {
                break;
            }
            consume();
        }
        expect(TokenType::GT);
    }
    save_block_pos(node);
    skip_block();
    if (next_is(TokenType::SEMICOLON)) {
        consume();
    }

    node->end_token = lookahead(-1);
    add_to_scope(node);
    return node;
}

Node *Parser::parse_struct_member(ContainerKind container_kind, Node *parent) {
    switch (container_kind) {
    case ContainerKind::Interface: {
        // Check for embed syntax (...interface_name)
        if (next_is(TokenType::ELLIPSIS)) {
            consume(); // consume ...
            auto embed_node = create_node(NodeType::VarDecl, get());
            embed_node->data.var_decl.is_embed = true;
            embed_node->data.var_decl.is_field = false;
            embed_node->data.var_decl.type = parse_type_expr(true);
            embed_node->name = "__embed"; // Give it a name for internal use
            expect(TokenType::SEMICOLON);
            return embed_node;
        }
        return parse_fn_decl(0);
    }
    default:
        if (next_is(TokenType::KW_IMPL)) {
            auto kw = expect(TokenType::KW_IMPL);
            // impl where T: Bound { ... } — alternative syntax for where blocks
            if (next_is(TokenType::KW_WHERE)) {
                consume(); // consume 'where'
                auto node = create_node(NodeType::ImplementBlock, kw);
                node->start_token = kw;
                // interface_types left empty for where-blocks
                do {
                    auto param = expect(TokenType::IDEN);
                    expect(TokenType::COLON);
                    do {
                        ast::WhereClause clause;
                        clause.param_name = param;
                        clause.bound_type = parse_type_expr(true);
                        node->data.implement_block.where_clauses.add(clause);
                    } while (next_is(TokenType::ADD) && (consume(), true));
                } while (next_is(TokenType::COMMA) && (consume(), true));
                expect(TokenType::LBRACE);
                while (get()->type != TokenType::RBRACE) {
                    if (get()->type == TokenType::END) {
                        error(get(), errors::UNEXPECTED_EOF);
                        break;
                    }
                    auto member = parse_fn_decl(FN_BODY_REQUIRED);
                    node->data.implement_block.members.add(member);
                    member->parent = node;
                }
                node->end_token = get();
                expect(TokenType::RBRACE);
                return node;
            }
            auto node = create_node(NodeType::ImplementBlock, kw);
            node->start_token = kw;
            do {
                node->data.implement_block.interface_types.add(parse_type_expr(true));
            } while (next_is(TokenType::COMMA) && (consume(), true));
            // impl Interface where T: Bound { ... } — conditional interface implementation
            if (next_is(TokenType::KW_WHERE)) {
                consume(); // consume 'where'
                do {
                    auto param = expect(TokenType::IDEN);
                    expect(TokenType::COLON);
                    do {
                        ast::WhereClause clause;
                        clause.param_name = param;
                        clause.bound_type = parse_type_expr(true);
                        node->data.implement_block.where_clauses.add(clause);
                    } while (next_is(TokenType::ADD) && (consume(), true));
                } while (next_is(TokenType::COMMA) && (consume(), true));
            }
            expect(TokenType::LBRACE);
            while (get()->type != TokenType::RBRACE) {
                if (get()->type == TokenType::END) {
                    error(get(), errors::UNEXPECTED_EOF);
                    break;
                }
                auto member = parse_fn_decl(FN_BODY_REQUIRED);
                node->data.implement_block.members.add(member);
                member->parent = node;
            }
            node->end_token = get();
            expect(TokenType::RBRACE);
            return node;
        }
        auto spec = parse_decl_spec();
        if (next_is(TokenType::KW_FUNC)) {
            return parse_fn_decl(FN_BODY_REQUIRED, spec);
        }
        return parse_var_decl(true, spec);
    }
}

void Parser::parse_enum_block(Node *node) {
    m_ctx->resolver->push_scope(node);
    for (auto param : node->data.enum_decl.type_params) {
        add_to_scope(param);
    }
    expect(TokenType::LBRACE);
    bool variants_ended = false;
    while (get()->type != TokenType::RBRACE) {
        if (get()->type == TokenType::END) {
            error(get(), errors::UNEXPECTED_EOF);
            return;
        }
        if (get()->type == TokenType::SEMICOLON) {
            consume();
            variants_ended = true;
        }
        if (variants_ended) {
            if (get()->type == (TokenType::KW_STRUCT)) {
                consume();
                auto struct_node = create_node(NodeType::StructDecl, get());
                struct_node->name = fmt::format("{}.BaseStruct", node->name);
                node->data.enum_decl.base_struct = struct_node;
                parse_struct_block(node->data.enum_decl.base_struct);
            } else {
                auto token = get();
                error(token, "expected 'struct', got '{}'", token->to_string());
                consume(); // consume the unexpected token to prevent infinite loop
                return;    // stop parsing this enum
            }
        } else {
            auto before_pos = m_toki;
            auto member = parse_enum_member(node);
            node->data.enum_decl.variants.add(member);

            // Error recovery: if we didn't advance, consume a token to avoid infinite loop
            if (m_toki == before_pos) {
                auto token = get();
                error(token, "unexpected token in enum declaration: '{}'", token->to_string());
                consume();
            }
        }
    }
    m_ctx->resolver->pop_scope();
    expect(TokenType::RBRACE);
}

void Parser::parse_struct_block(Node *node) {
    m_ctx->resolver->push_scope(node);
    for (auto param : node->data.struct_decl.type_params) {
        add_to_scope(param);
    }
    expect(TokenType::LBRACE);
    while (get()->type != TokenType::RBRACE) {
        if (get()->type == TokenType::END) {
            error(get(), errors::UNEXPECTED_EOF);
            return;
        }
        auto before_pos = m_toki;
        auto member = parse_struct_member(node->data.struct_decl.kind, node);
        node->data.struct_decl.members.add(member);
        member->parent = node;

        // Error recovery: if we didn't advance, consume a token to avoid infinite loop
        if (m_toki == before_pos) {
            auto token = get();
            error(token, "unexpected token in struct declaration: '{}'", token->to_string());
            consume();
        }
    }
    m_ctx->resolver->pop_scope();
    expect(TokenType::RBRACE);
}

Node *Parser::parse_construct_expr() {
    Node *node = create_node(NodeType::ConstructExpr, get());
    if (next_is(TokenType::KW_NEW)) {
        consume();
        node->data.construct_expr.is_new = true;
        if (!next_is(TokenType::LBRACE)) {
            node->data.construct_expr.type = parse_type_expr(true);
        }
    } else if (!next_is(TokenType::LBRACE)) {
        // We have a type expression like Array<int>{...}
        node->data.construct_expr.type = parse_type_expr(true);
    }
    expect(TokenType::LBRACE);
    Token *token;
    bool field_started = false;
    for (;;) {
        token = get();
        if (token->type == TokenType::END) {
            error(token, errors::UNEXPECTED_EOF);
            return node;
        }
        if (token->type == TokenType::RBRACE) {
            break;
        }

        if (token->type == TokenType::ELLIPSIS) {
            // spread: ...expr
            consume();
            if (node->data.construct_expr.spread_expr) {
                error(token, "only one spread expression allowed");
            }
            node->data.construct_expr.spread_expr = parse_child_expr_construct(false, node);
            if (!at_comma(TokenType::RBRACE))
                break;
            consume();
            continue;
        }

        if (token->type == TokenType::COLON && lookahead(1)->type == TokenType::IDEN) {
            // shorthand field initializer: :field (desugars to field: field)
            field_started = true;
            consume(); // consume ':'
            auto field = get();
            auto value = parse_identifier();
            auto field_init = create_node(NodeType::FieldInitExpr, field);
            field_init->data.field_init_expr.token = field;
            field_init->data.field_init_expr.field = field;
            field_init->data.field_init_expr.value = value;
            node->data.construct_expr.field_inits.add(field_init);
        } else if (token->type == TokenType::COLON) {
            // Incomplete shorthand: just ':' without identifier (mid-edit)
            // Consume and continue so the construct expression closes cleanly
            consume();
            if (!at_comma(TokenType::RBRACE))
                break;
            consume();
            continue;
        } else if (token->type == TokenType::IDEN && lookahead(1)->type == TokenType::COLON) {
            // field initializer: field: value
            field_started = true;
            consume();
            auto field = token;
            expect(TokenType::COLON);
            auto value = parse_child_expr_construct(false, node);
            auto field_init = create_node(NodeType::FieldInitExpr, field);
            field_init->data.field_init_expr.token = token;
            field_init->data.field_init_expr.field = field;
            field_init->data.field_init_expr.value = value;
            node->data.construct_expr.field_inits.add(field_init);
        } else {
            // argument value
            if (field_started) {
                unexpected(token);
            }

            // Check for problematic tokens that could cause infinite recursion
            if (token->type == TokenType::LBRACK || token->type == TokenType::RBRACE ||
                token->type == TokenType::END) {
                unexpected(token);
                break; // Stop processing this construct expression
            }

            auto expr = parse_child_expr_construct(false, node);
            if (expr && expr->type != NodeType::Error) {
                node->data.construct_expr.items.add(expr);
            } else {
                // If parse failed, consume token and continue
                if (get()->type != TokenType::RBRACE && get()->type != TokenType::END) {
                    consume();
                }
            }
        }
        if (!at_comma(TokenType::RBRACE)) {
            break;
        }
        consume();
    }
    node->end_token = expect(TokenType::RBRACE);
    return node;
}

Node *Parser::parse_prefix_expr() {
    auto node = create_node(NodeType::PrefixExpr, get());
    auto tok = get();
    consume();
    node->data.prefix_expr.prefix = tok;
    if (tok->type == TokenType::KW_SIZEOF) {
        // Handle prefix * dereference: sizeof *expr
        if (next_is(TokenType::MUL)) {
            auto star_tok = get();
            consume();
            auto inner = parse_type_expr(true);
            auto deref = create_node(NodeType::UnaryOpExpr, star_tok);
            deref->data.unary_op_expr.op_type = TokenType::MUL;
            deref->data.unary_op_expr.op1 = inner;
            node->data.prefix_expr.expr = deref;
        } else {
            node->data.prefix_expr.expr = parse_type_expr(true);
        }
    } else {
        node->data.prefix_expr.expr = parse_child_expr_construct(false, node);
    }
    return node;
}

Node *Parser::parse_dot_expr(Node *expr, bool is_optional_chain) {
    auto dot = is_optional_chain ? expect(TokenType::QUES_DOT) : expect(TokenType::DOT);
    auto node = create_node(NodeType::DotExpr, dot);
    node->data.dot_expr.expr = expr;
    node->data.dot_expr.is_optional_chain = is_optional_chain;
    // Allow integer literals for tuple field access: expr.0, expr.1
    Token *field;
    if (get()->type == TokenType::INT) {
        field = read();
        field->str = std::to_string(field->val.i);
    } else {
        field = expect_identifier();
    }
    field->semantic_node = node;
    node->data.dot_expr.field = field;

    if (next_is(TokenType::DOT)) {
        return parse_dot_expr(node);
    }
    if (next_is(TokenType::QUES_DOT)) {
        return parse_dot_expr(node, true);
    }
    return node;
}

Node *Parser::parse_index_expr(Node *expr) {
    auto lb = expect(TokenType::LBRACK);

    // Check for slice syntax: a[..end] or a[..]
    if (next_is(TokenType::DOT_DOT)) {
        consume();
        auto node = create_node(NodeType::SliceExpr, lb);
        node->data.slice_expr.expr = expr;
        if (!next_is(TokenType::RBRACK)) {
            node->data.slice_expr.end = parse_child_expr_construct(false, node);
        }
        expect(TokenType::RBRACK);
        return node;
    }

    auto first = parse_child_expr_construct(false, nullptr);

    // Check for slice syntax: a[start..end] or a[start..]
    if (next_is(TokenType::DOT_DOT)) {
        consume();
        auto node = create_node(NodeType::SliceExpr, lb);
        node->data.slice_expr.expr = expr;
        node->data.slice_expr.start = first;
        if (!next_is(TokenType::RBRACK)) {
            node->data.slice_expr.end = parse_child_expr_construct(false, node);
        }
        expect(TokenType::RBRACK);
        return node;
    }

    // Regular index expression
    auto node = create_node(NodeType::IndexExpr, lb);
    node->data.index_expr.expr = expr;
    node->data.index_expr.subscript = first;
    expect(TokenType::RBRACK);
    return node;
}

Node *Parser::parse_typedef() {
    auto token = expect(TokenType::KW_TYPEDEF);
    auto node = create_node(NodeType::TypedefDecl, token);
    auto iden = expect(TokenType::IDEN);
    iden->semantic_node = node;
    node->name = iden->str;
    node->data.typedef_decl.identifier = iden;
    auto &params = node->data.typedef_decl.type_params;
    if (next_is(TokenType::LT)) {
        expect(TokenType::LT);
        Token *param_token;
        for (;;) {
            param_token = get();
            if (param_token->type == TokenType::END) {
                error(param_token, errors::UNEXPECTED_EOF);
                return node;
            }
            if (param_token->type == TokenType::GT) {
                break;
            }
            bool is_variadic = param_token->type == TokenType::ELLIPSIS;
            if (is_variadic) consume(); // skip past ...
            auto param_iden = expect(TokenType::IDEN);
            auto param_node = create_node(NodeType::TypeParam, param_iden);
            param_node->data.type_param.is_variadic = is_variadic;
            if (next_is(TokenType::COLON)) {
                consume();
                do {
                    param_node->data.type_param.type_bounds.add(parse_type_expr(true));
                } while (next_is(TokenType::ADD) && (consume(), true));
            }
            if (next_is(TokenType::ASS)) {
                consume();
                param_node->data.type_param.default_type = parse_type_expr(true);
            }
            param_node->data.type_param.index = params.len;
            param_node->data.type_param.source_decl = node;
            params.add(param_node);
            if (!at_comma(TokenType::GT)) {
                break;
            }
            consume();
        }
        expect(TokenType::GT);
    }
    if (params.len > 0) {
        m_ctx->resolver->push_scope(node);
        for (auto type_param : params) {
            m_ctx->resolver->declare_symbol(type_param->name, type_param);
        }
    }
    expect(TokenType::ASS);
    node->data.typedef_decl.type = parse_type_expr(true);
    if (params.len > 0) {
        m_ctx->resolver->pop_scope();
    }
    add_to_scope(node, iden->str);
    expect(TokenType::SEMICOLON);
    return node;
}

Node *Parser::parse_enum_member(Node *parent) {
    auto iden = expect(TokenType::IDEN);
    auto node = create_node(NodeType::EnumVariant, iden);
    node->data.enum_variant.name = iden;
    node->data.enum_variant.parent = parent;
    if (next_is(TokenType::LPAREN)) {
        consume();
        auto struct_node = create_node(NodeType::StructDecl, iden);
        struct_node->data.struct_decl.kind = ContainerKind::Struct;
        struct_node->parent = node;
        node->data.enum_variant.struct_body = struct_node;
        node->data.enum_variant.is_tuple_form = true;
        long field_index = 0;
        while (get()->type != TokenType::RPAREN) {
            if (get()->type == TokenType::END) {
                error(get(), errors::UNEXPECTED_EOF);
                break;
            }
            auto field_type = parse_type_expr(true);
            node->data.enum_variant.tuple_types.add(field_type);

            auto field = create_node(NodeType::VarDecl, field_type->token ? field_type->token : iden);
            field->parent = struct_node;
            field->name = std::to_string(field_index);
            field->data.var_decl.identifier = field->token;
            field->data.var_decl.type = field_type;
            field->data.var_decl.is_field = true;
            struct_node->data.struct_decl.members.add(field);
            field_index++;

            if (next_is(TokenType::COMMA)) {
                consume();
            } else if (!next_is(TokenType::RPAREN)) {
                auto token = get();
                error(token, "expected ',' after tuple enum member type, got '{}'",
                      token->to_string());
                consume();
            }
        }
        expect(TokenType::RPAREN);
    } else if (next_is(TokenType::LBRACE)) {
        consume();
        auto struct_node = create_node(NodeType::StructDecl, iden);
        struct_node->parent = node;
        node->data.enum_variant.struct_body = struct_node;
        while (get()->type != TokenType::RBRACE) {
            if (get()->type == TokenType::END) {
                error(get(), errors::UNEXPECTED_EOF);
                break;
            }
            auto before_pos = m_toki;
            auto member = parse_struct_member(node->data.struct_decl.kind, struct_node);
            struct_node->data.struct_decl.members.add(member);

            // Error recovery: if we didn't advance, consume a token to avoid infinite loop
            if (m_toki == before_pos) {
                auto token = get();
                error(token, "unexpected token in struct declaration: '{}'", token->to_string());
                consume();
            }
        }
        expect(TokenType::RBRACE);
    }

    if (next_is(TokenType::ASS)) {
        consume();
        node->data.enum_variant.value = parse_expr_clause(false);
    }

    if (!next_is(TokenType::RBRACE) && !next_is(TokenType::SEMICOLON) && !next_is(TokenType::END)) {
        if (next_is(TokenType::COMMA)) {
            consume(); // consume the comma
        } else {
            // Missing comma - consume the unexpected token and report error
            auto token = get();
            error(token, "expected ',' after enum member, got '{}'", token->to_string());
            consume();
        }
    }

    return node;
}

Node *Parser::parse_extern_decl(DeclSpec *decl_spec) {
    auto kw = expect(TokenType::KW_EXTERN);
    auto type = expect(TokenType::STRING);

    auto node = create_node(NodeType::ExternDecl, kw);
    node->data.extern_decl = {};
    node->data.extern_decl.type = type;
    node->data.extern_decl.decl_spec = decl_spec;

    auto &members = node->data.extern_decl.members;
    auto &imports = node->data.extern_decl.imports;
    auto &exports = node->data.extern_decl.exports;
    expect(TokenType::LBRACE);
    Token *token;
    for (;;) {
        token = get();
        if (token->type == TokenType::END) {
            error(token, errors::UNEXPECTED_EOF);
            return node;
        }
        if (token->type == TokenType::RBRACE) {
            consume();
            node->end_token = token;
            break;
        }

        // Handle import statements: extern "C" { import "header.h" as h; }
        if (token->type == TokenType::KW_IMPORT) {
            auto import_node = parse_import_decl();
            imports.add(import_node);
            continue;
        }

        // Handle export statements: extern "C" { export {strlen} from "string.h"; }
        if (token->type == TokenType::KW_EXPORT) {
            auto export_node = parse_export_decl();
            exports.add(export_node);
            continue;
        }

        // Handle inline function declarations
        auto fn = parse_fn_decl(FN_BODY_NONE);
        fn->data.fn_def.decl_spec->flags |= DECL_EXTERN;
        if (decl_spec) {
            fn->data.fn_def.decl_spec->flags |= (decl_spec->flags & (DECL_PRIVATE | DECL_UNSAFE | DECL_EXPORTED));
        }
        members.add(fn);

        // add export if exported
        if (fn->data.fn_def.decl_spec->is_exported()) {
            m_ctx->module->exports.add(fn);
        }
    }
    return node;
}

Node *Parser::parse_import_decl() {
    auto kw = expect(TokenType::KW_IMPORT);
    auto node = create_node(NodeType::ImportDecl, kw);
    node->data.import_decl = {};

    // Support both syntaxes:
    // New: import * as mod from "./module"
    // New: import {X, Y} from "./module"
    // New: import mod from "./module" (JS-style default import, treated as namespace)
    // Old: import "./module" as mod
    // Old: import "./module" {X, Y}

    if (next_is(TokenType::IDEN)) {
        auto iden = expect(TokenType::IDEN);

        // Check if followed by * for pattern import: import str* from "module"
        if (next_is(TokenType::MUL)) {
            consume(); // consume the *

            // Convert to pattern import: {str*} from "module"
            std::string pattern = iden->str + "*";
            iden->str = pattern;

            auto member = create_node(NodeType::ImportSymbol, iden);
            member->data.import_symbol.name = iden;
            node->data.import_decl.symbols.add(member);
            member->data.import_symbol.import = node;
            add_to_scope(member, pattern);

            expect(TokenType::KW_FROM);
            node->data.import_decl.path = expect(TokenType::STRING);
        } else {
            // Regular namespace import: import mod from "./module"
            node->data.import_decl.alias = iden;
            add_to_scope(node, iden->str);
            expect(TokenType::KW_FROM);
            node->data.import_decl.path = expect(TokenType::STRING);
        }
    } else if (next_is(TokenType::MUL)) {
        // import * as mod from "./module" OR import * from "./module" (wildcard)
        auto star = expect(TokenType::MUL);
        node->data.import_decl.match_all = star;

        // Check if this is: import * from "module" (wildcard without alias)
        if (next_is(TokenType::KW_FROM)) {
            consume();
            node->data.import_decl.path = expect(TokenType::STRING);
            // No alias - symbols imported directly into scope
        }
        // Error recovery: if we see STRING instead of 'as', the user likely meant the old syntax
        else if (next_is(TokenType::STRING)) {
            if (!m_ctx->format_mode) {
                error(get(), "invalid import syntax: expected 'as' or 'from' after '*'");
            }
            // Recover: treat as old syntax "import './module' as alias"
            node->data.import_decl.path = expect(TokenType::STRING);
            if (next_is(TokenType::KW_AS)) {
                consume();
                auto iden = expect(TokenType::IDEN);
                node->data.import_decl.alias = iden;
                add_to_scope(node, iden->str);
            }
        } else {
            // import * as alias from "module"
            expect(TokenType::KW_AS);
            auto iden = expect(TokenType::IDEN);
            node->data.import_decl.alias = iden;
            add_to_scope(node, iden->str);
            expect(TokenType::KW_FROM);
            node->data.import_decl.path = expect(TokenType::STRING);
        }
    } else if (next_is(TokenType::LBRACE)) {
        // import {X, Y} from "./module" OR import "./module" {X, Y} (after path parsed)
        consume();
        while (!next_is(TokenType::RBRACE) && !next_is(TokenType::END) &&
               !next_is(TokenType::SEMICOLON)) {
            // Handle wildcard patterns: SDL_*, *_init, etc.
            Token *pattern_token = nullptr;
            std::string pattern_str;

            // Check for leading *
            if (next_is(TokenType::MUL)) {
                auto star = expect(TokenType::MUL);
                pattern_str = "*";
                pattern_token = star;

                // Optionally followed by identifier
                if (next_is(TokenType::IDEN)) {
                    auto iden = expect(TokenType::IDEN);
                    pattern_str += iden->str;
                }
            } else if (next_is(TokenType::IDEN)) {
                auto iden = expect(TokenType::IDEN);
                pattern_str = iden->str;
                pattern_token = iden;

                // Optionally followed by *
                if (next_is(TokenType::MUL)) {
                    consume();
                    pattern_str += "*";
                }
            } else {
                // Skip unexpected token to avoid infinite loop
                consume();
                continue;
            }

            // Create a synthetic token with the full pattern
            auto iden = pattern_token;
            iden->str = pattern_str;

            auto member = create_node(NodeType::ImportSymbol, iden);
            member->data.import_symbol.name = iden;
            auto name_iden = iden;
            if (next_is(TokenType::KW_AS)) {
                consume();
                name_iden = expect(TokenType::IDEN);
                member->data.import_symbol.alias = name_iden;
            }
            if (!next_is(TokenType::RBRACE) && !next_is(TokenType::SEMICOLON)) {
                if (next_is(TokenType::COMMA)) {
                    consume();
                } else {
                    // Unexpected token, skip to avoid infinite loop
                    consume();
                }
            }
            node->data.import_decl.symbols.add(member);
            member->data.import_symbol.import = node;
            add_to_scope(member, name_iden->get_name());
        }
        expect(TokenType::RBRACE);
        expect(TokenType::KW_FROM);
        node->data.import_decl.path = expect(TokenType::STRING);
    } else if (next_is(TokenType::STRING)) {
        // Old syntax: import "./module" as mod OR import "./module" {X, Y}
        node->data.import_decl.path = expect(TokenType::STRING);
        if (next_is(TokenType::KW_AS)) {
            consume();
            auto iden = expect(TokenType::IDEN);
            node->data.import_decl.alias = iden;
            add_to_scope(node, iden->str);
        } else if (next_is(TokenType::LBRACE)) {
            consume();
            while (!next_is(TokenType::RBRACE) && !next_is(TokenType::END) &&
                   !next_is(TokenType::SEMICOLON)) {
                // Skip if we encounter unexpected tokens
                if (!next_is(TokenType::IDEN) && !next_is(TokenType::RBRACE)) {
                    consume();
                    continue;
                }

                auto iden = expect(TokenType::IDEN);
                auto member = create_node(NodeType::ImportSymbol, iden);
                member->data.import_symbol.name = iden;
                auto name_iden = iden;
                if (next_is(TokenType::KW_AS)) {
                    consume();
                    name_iden = expect(TokenType::IDEN);
                    member->data.import_symbol.alias = name_iden;
                }
                if (!next_is(TokenType::RBRACE) && !next_is(TokenType::SEMICOLON)) {
                    if (next_is(TokenType::COMMA)) {
                        consume();
                    } else {
                        consume(); // Skip unexpected token
                    }
                }
                node->data.import_decl.symbols.add(member);
                member->data.import_symbol.import = node;
                add_to_scope(member, name_iden->get_name());
            }
            expect(TokenType::RBRACE);
        }
    } else {
        if (!m_ctx->format_mode) {
            error(get(), "expected identifier, '*', '{{', or module path in import declaration");
        }
        // Error recovery: consume tokens until we find a semicolon or reasonable import syntax
        // For now, just create a dummy path to prevent null pointer crashes
        node->data.import_decl.path = kw; // Use the import keyword as a dummy path token
    }

    // Ensure we always have a path set to prevent crashes in the resolver
    if (!node->data.import_decl.path) {
        node->data.import_decl.path = kw; // Use import keyword as fallback
    }

    expect(TokenType::SEMICOLON);
    return node;
}

Node *Parser::parse_export_decl() {
    auto kw = expect(TokenType::KW_EXPORT);
    auto node = create_node(NodeType::ExportDecl, kw);
    node->data.export_decl = {};
    node->data.export_decl.decl_spec = m_ctx->allocator->create_decl_spec();

    // Only support: export * from "./module" or export {X, Y} from "./module"
    if (next_is(TokenType::MUL)) {
        auto ellipsis = get();
        consume();
        node->data.export_decl.match_all = ellipsis;
        expect(TokenType::KW_FROM);
        node->data.export_decl.path = expect(TokenType::STRING);
    } else if (next_is(TokenType::IDEN)) {
        // Check if this is a pattern export: export str* from "module"
        auto iden = expect(TokenType::IDEN);

        if (next_is(TokenType::MUL)) {
            consume(); // consume the *

            // Convert to pattern export: {str*} from "module"
            std::string pattern = iden->str + "*";
            iden->str = pattern;

            auto member = create_node(NodeType::ImportSymbol, iden);
            member->data.import_symbol.name = iden;
            node->data.export_decl.symbols.add(member);
            member->data.import_symbol.import = node;

            expect(TokenType::KW_FROM);
            node->data.export_decl.path = expect(TokenType::STRING);
        } else {
            // Not a pattern, treat as error or old syntax
            if (!m_ctx->format_mode) {
                error(iden, "expected '*' or 'from' after identifier in export declaration");
            }
            node->data.export_decl.path = iden; // Use as dummy path
        }
    } else if (next_is(TokenType::LBRACE)) {
        consume();
        while (!next_is(TokenType::RBRACE) && !next_is(TokenType::END) &&
               !next_is(TokenType::SEMICOLON)) {
            // Handle wildcard patterns: SDL_*, *_init, etc.
            Token *pattern_token = nullptr;
            std::string pattern_str;

            // Check for leading *
            if (next_is(TokenType::MUL)) {
                auto star = expect(TokenType::MUL);
                pattern_str = "*";
                pattern_token = star;

                // Optionally followed by identifier
                if (next_is(TokenType::IDEN)) {
                    auto iden = expect(TokenType::IDEN);
                    pattern_str += iden->str;
                }
            } else if (next_is(TokenType::IDEN)) {
                auto iden = expect(TokenType::IDEN);
                pattern_str = iden->str;
                pattern_token = iden;

                // Optionally followed by *
                if (next_is(TokenType::MUL)) {
                    consume();
                    pattern_str += "*";
                }
            } else {
                // Skip unexpected token to avoid infinite loop
                consume();
                continue;
            }

            // Create a synthetic token with the full pattern
            auto iden = pattern_token;
            iden->str = pattern_str;

            auto member = create_node(NodeType::ImportSymbol, iden);
            member->data.import_symbol.name = iden;
            auto name_iden = iden;
            if (next_is(TokenType::KW_AS)) {
                consume();
                name_iden = expect(TokenType::IDEN);
                member->data.import_symbol.alias = name_iden;
            }
            if (!next_is(TokenType::RBRACE) && !next_is(TokenType::SEMICOLON)) {
                if (next_is(TokenType::COMMA)) {
                    consume();
                } else {
                    consume(); // Skip unexpected token
                }
            }
            node->data.export_decl.symbols.add(member);
            member->data.import_symbol.import = node;
            add_to_scope(member, name_iden->get_name());
        }
        expect(TokenType::RBRACE);
        expect(TokenType::KW_FROM);
        node->data.export_decl.path = expect(TokenType::STRING);
    } else {
        if (!m_ctx->format_mode) {
            error(get(), "expected '*' or '{{' in export declaration");
        }
        // Error recovery: use export keyword as dummy path to prevent crashes
        node->data.export_decl.path = kw;
    }

    // Ensure we always have a path set to prevent crashes
    if (!node->data.export_decl.path) {
        node->data.export_decl.path = kw;
    }

    expect(TokenType::SEMICOLON);
    return node;
}

bool Parser::looks_like_case_pattern_clause() {
    int offset = 0;
    auto token = lookahead(offset);
    if (!token || token->type != TokenType::IDEN) {
        return false;
    }

    for (;;) {
        token = lookahead(offset);
        if (!token || token->type != TokenType::IDEN) {
            return false;
        }
        offset++;

        if (lookahead(offset)->type == TokenType::LT) {
            int depth = 0;
            do {
                token = lookahead(offset);
                if (!token || token->type == TokenType::END) {
                    return false;
                }
                if (token->type == TokenType::LT) {
                    depth++;
                } else if (token->type == TokenType::GT) {
                    depth--;
                }
                offset++;
            } while (depth > 0);
        }

        token = lookahead(offset);
        if (!token) {
            return false;
        }
        if (token->type == TokenType::DOT) {
            offset++;
            continue;
        }
        return token->type == TokenType::LPAREN || token->type == TokenType::LBRACE ||
               token->type == TokenType::LBRACK;
    }
}

Node *Parser::parse_switch_expr(bool require_value) {
    auto kw = expect(TokenType::KW_SWITCH);
    auto node = create_node(NodeType::SwitchExpr, kw);
    auto scope = m_ctx->resolver->push_scope(node);

    node->data.switch_expr.expr = parse_expr();

    // Handle case where expression is missing or invalid
    if (!node->data.switch_expr.expr) {
        // Create a dummy literal node to avoid null pointer crashes
        node->data.switch_expr.expr = create_node(NodeType::LiteralExpr, kw);
    }

    if (node->data.switch_expr.expr->type == NodeType::TypeInfoExpr) {
        node->data.switch_expr.is_type_switch = true;
        node->data.switch_expr.expr = node->data.switch_expr.expr->data.type_info_expr.expr;
    }

    expect(TokenType::LBRACE);
    while (!next_is(TokenType::RBRACE) && !next_is(TokenType::END) &&
           !next_is(TokenType::SEMICOLON)) {
        auto case_expr = parse_case_expr(node->data.switch_expr.is_type_switch);
        // Only add valid case expressions
        if (case_expr) {
            node->data.switch_expr.cases.add(case_expr);
        }
        if (!at_comma(TokenType::RBRACE)) {
            break;
        }
        consume();
    }
    expect(TokenType::RBRACE);
    m_ctx->resolver->pop_scope();
    if (require_value && !switch_expr_returns_value(node)) {
        error(node->token,
              "switch used as an expression must have every case produce a value");
    }
    return node;
}

Node *Parser::parse_type_info_expr(Node *expr) {
    auto dot = expect(TokenType::DOT);
    expect(TokenType::LPAREN);
    auto type_tok = get();
    if (type_tok->type != TokenType::IDEN || type_tok->str != "type") {
        error(type_tok, "expected 'type' after '.('");
    } else {
        consume();
    }
    expect(TokenType::RPAREN);

    auto node = create_node(NodeType::TypeInfoExpr, dot);
    node->data.type_info_expr.expr = expr;
    return node;
}

Node *Parser::parse_case_expr(bool is_type_switch) {
    Node *node = nullptr;
    if (next_is(TokenType::KW_ELSE)) {
        auto token = read();
        node = create_node(NodeType::CaseExpr, token);
        node->data.case_expr.is_else = true;
    } else {
        Node *expr = nullptr;
        Node *pattern = nullptr;
        if (!is_type_switch && looks_like_case_pattern_clause()) {
            expr = parse_type_expr(true);
            if (starts_destructure_pattern()) {
                pattern = parse_any_destructure_pattern(VarKind::Mutable);
            }
        } else {
            expr = is_type_switch ? parse_type_expr(true) : parse_expr();
        }

        node = create_node(NodeType::CaseExpr, expr->token);
        node->data.case_expr.clauses = {expr};
        node->data.case_expr.destructure_pattern = pattern;
        if (pattern && next_is(TokenType::COMMA)) {
            error(get(), "switch destructure patterns cannot be combined with multiple clauses");
        } else if (next_is(TokenType::COMMA)) {
            consume();
            while (!next_is(TokenType::ARROW) && !next_is(TokenType::END)) {
                auto expr = is_type_switch ? parse_type_expr(true) : parse_expr();
                node->data.case_expr.clauses.add(expr);
                if (!at_comma(TokenType::ARROW)) {
                    break;
                }
                consume();
            }
        }
    }

    auto arrow = expect(TokenType::ARROW);
    Scope *case_scope = nullptr;
    if (node->data.case_expr.destructure_pattern) {
        case_scope = m_ctx->resolver->push_scope(node);
        register_destructure_bindings(this, node->data.case_expr.destructure_pattern);
    }
    node->data.case_expr.body = parse_block(case_scope, arrow);
    if (case_scope) {
        m_ctx->resolver->pop_scope();
    }
    return node;
}
