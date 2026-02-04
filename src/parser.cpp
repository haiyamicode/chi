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

static string get_token_type_repr(TokenType token_type) {
    switch (token_type) {
    case TokenType::IDEN:
        return "an identifier";
    case TokenType::CHAR:
        return "character";
    case TokenType::STRING:
        return "string literal";
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

    case TokenType::QUES:
        return TERNARY_PREC;

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
    case TokenType::KW_EXTERN:
    case TokenType::KW_IMPORT:
    case TokenType::KW_EXPORT:
    case TokenType::KW_TYPEDEF:
    case TokenType::KW_PRIVATE:
    case TokenType::KW_PROTECTED:
    case TokenType::KW_STATIC:
    case TokenType::KW_ASYNC:
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
    if (token->type == TokenType::IDEN || token->type == TokenType::KW_THIS_TYPE) {
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
            token->type != TokenType::KW_STATIC) {
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
    default:
        break;
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
    case TokenType::KW_ASYNC:
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
        return parse_var_decl(false);
    case TokenType::KW_FUNC: {
        return parse_fn_decl(FN_BODY_REQUIRED, decl_spec);
    }
    case TokenType::KW_EXTERN:
        return parse_extern_decl();
    case TokenType::KW_IMPORT:
        return parse_import_decl();
    case TokenType::KW_EXPORT:
        return parse_export_decl();
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
            *iden = expect(TokenType::IDEN);
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
    if (!decl && token->type != TokenType::KW_THIS_TYPE) {
        error(token, errors::UNDECLARED, node->name);
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
    case TokenType::XOR:
        return SigilKind::Box;
    case TokenType::QUES:
        return SigilKind::Optional;
    default:
        return {};
    }
}

Node *Parser::parse_type_expr(bool type_only) {
    if (!type_only) {
        expect(TokenType::COLON);
    }

    array<Node *> sigil_nodes;
    for (;;) {
        auto token = get();
        auto has_wrapping = false;
        if (auto sigil_kind = get_sigil_kind(token->type)) {
            consume();

            if (sigil_kind == SigilKind::Reference && next_is(TokenType::KW_MUT)) {
                consume();
                expect(TokenType::LT);
                has_wrapping = true;
                sigil_kind = SigilKind::MutRef;
            }

            if (next_is(TokenType::LT)) {
                has_wrapping = true;
                consume();
            }
            auto node = create_node(NodeType::TypeSigil, token);
            node->data.sigil_type.sigil = *sigil_kind;
            node->data.sigil_type.has_wrapping = has_wrapping;
            sigil_nodes.add(node);

        } else {
            break;
        }
    }

    ast::Node *node = nullptr;
    auto token = get();
    if (token->type == TokenType::KW_FUNC) {
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
                auto param = parse_type_expr(true);
                subtype.args.add(param);
                if (!at_comma(TokenType::GT)) {
                    break;
                }
                consume();
            }
            expect(TokenType::GT);
        }
    }

    for (int i = int(sigil_nodes.len) - 1; i >= 0; --i) {
        auto parent = sigil_nodes[i];
        parent->data.sigil_type.type = node;
        node = parent;

        if (parent->data.sigil_type.has_wrapping) {
            expect(TokenType::GT);
        }
    }

    return node;
}

Node *Parser::parse_fn_lambda() {
    auto token = expect(TokenType::KW_FUNC);
    auto fn = create_node(NodeType::FnDef, token);

    // Parse optional by-value capture list: func [x, y] (params) { ... }
    if (next_is(TokenType::LBRACK)) {
        read(); // consume [
        while (!next_is(TokenType::RBRACK)) {
            auto iden = expect(TokenType::IDEN);
            fn->data.fn_def.value_captures.add(iden->str);
            if (!next_is(TokenType::COMMA)) break;
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

        // Create scope and add params
        auto scope = m_ctx->resolver->push_scope(fn);
        auto &fn_proto = proto->data.fn_proto;
        for (auto param : fn_proto.params) {
            add_to_scope(param);
            param->parent_fn = fn;
        }

        // Parse expression and wrap in return statement
        auto expr = parse_expr();
        auto ret = create_node(NodeType::ReturnStmt, arrow);
        ret->data.return_stmt.expr = expr;

        // Create block containing the return statement
        auto block = create_node(NodeType::Block, arrow);
        block->data.block.scope = scope;
        block->data.block.statements.add(ret);
        block->data.block.is_arrow = true;
        fn->data.fn_def.body = block;

        m_ctx->resolver->pop_scope();
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
    bool is_const = false;
    if (!as_field) {
        if (next_is(TokenType::KW_LET)) {
            is_const = true;
            consume();
        } else {
            expect(TokenType::KW_VAR);
        }
    }

    bool is_embed = false;
    if (as_field && next_is(TokenType::ELLIPSIS)) {
        is_embed = true;
        consume();
    }
    auto iden = expect(TokenType::IDEN);
    if (iden->type != TokenType::IDEN) {
        return create_error_node();
    }
    auto node = create_node(NodeType::VarDecl, iden);
    node->data.var_decl.identifier = iden;
    node->data.var_decl.is_const = is_const;
    node->data.var_decl.is_field = as_field;
    node->data.var_decl.decl_spec = decl_spec;
    node->data.var_decl.is_embed = is_embed;

    if (!as_field) {
        node->parent_fn = get_scope()->find_parent(NodeType::FnDef);
        // In malformed code, parent_fn might be null
        if (!node->parent_fn) {
            error(iden, "variable declaration outside of function");
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

Node *Parser::parse_fn_proto(Token *token, Node *fn_node) {
    auto proto = create_node(NodeType::FnProto, token);
    proto->name = token->get_name();

    // Push a scope for function prototype parsing
    auto proto_scope = m_ctx->resolver->push_scope(proto);

    // Parse type parameters like <T, U>
    auto &type_params = proto->data.fn_proto.type_params;
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

            auto param_iden = expect(TokenType::IDEN);
            auto param_node = create_node(NodeType::TypeParam, param_iden);
            param_node->name = param_iden->str; // Set the name explicitly
            param_node->data.type_param.index = type_params.len;
            param_node->data.type_param.source_decl = fn_node;

            // Check for colon syntax for type bounds: T: SomeInterface
            if (next_is(TokenType::COLON)) {
                consume(); // consume the colon
                param_node->data.type_param.type_bound = parse_type_expr(true);
            }

            type_params.add(param_node);

            // Add type parameter to scope
            add_to_scope(param_node);

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
    if (!next_is(TokenType::LBRACE) && !next_is(TokenType::SEMICOLON) && !next_is(TokenType::ARROW)) {
        proto->data.fn_proto.return_type = parse_type_expr(true);
    }

    // Pop the prototype scope
    m_ctx->resolver->pop_scope();
    return proto;
}

Node *Parser::parse_fn_type(Token *func) {
    auto proto = create_node(NodeType::FnProto, func);
    auto &data = proto->data.fn_proto;
    data.is_type_expr = true;

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
                             next_is(TokenType::ASS);  // For default values in struct fields
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
        consume();  // consume '='
        param->data.param_decl.default_value = parse_expr();
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
        bool as_expr = false;
        auto start_token = token;
        auto stmt = parse_stmt(&as_expr);
        node->data.block.return_expr = stmt;
        stmt->start_token = start_token;
        stmt->end_token = lookahead(-1);

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

Node *Parser::parse_stmt(bool *as_expr) {
    auto token = get();
    switch (token->type) {
    case TokenType::KW_IF:
        return parse_if_stmt();

    case TokenType::KW_FOR:
        return parse_for_stmt();

    case TokenType::KW_WHILE:
        return parse_while_stmt();

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
    case TokenType::KW_SWITCH:
    case TokenType::KW_TRY:
    case TokenType::KW_AWAIT: {
        if (next_is(TokenType::KW_VAR) || next_is(TokenType::KW_LET)) {
            return parse_var_decl(false);
        }

        auto expr = parse_expr();
        if (next_is(TokenType::SEMICOLON)) {
            consume();
        } else {
            *as_expr = true;
        }
        return expr;
    }

    case TokenType::KW_RETURN:
        return parse_return_stmt();

    case TokenType::KW_CONTINUE:
    case TokenType::KW_BREAK:
        return parse_branch_stmt();

    case TokenType::LBRACE:
        return parse_block();

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

    case TokenType::KW_AS: {
        auto node = create_node(NodeType::CastExpr, read());
        node->data.cast_expr.dest_type = parse_type_expr(true);
        node->data.cast_expr.expr = lhs;
        return node;
    }

    default:
        return lhs;
    }
    auto node = create_node(NodeType::BinOpExpr, token);
    node->data.bin_op_expr.op1 = lhs;
    node->data.bin_op_expr.op_type = token->type;
    node->data.bin_op_expr.op2 = parse_expr_clause(false);
    return node;
}

Node *Parser::parse_expr_clause(bool lhs) { return parse_binary_expr(lhs, nullptr, DEFAULT_PREC); }

Node *Parser::parse_child_expr_construct(bool lhs, Node *parent) {
    // Try to parse as type construct expression if that's possible (e.g., Array<int>{1, 2, 3})
    if (!lhs && is_construct_expr_with_type()) {
        return parse_construct_expr();
    }

    // Fall back to normal expression parsing
    return parse_child_expr(lhs, parent);
}

Node *Parser::parse_binary_expr(bool lhs, Node *parent, int prec) {
    auto op1 = parse_unary_expr(lhs, parent);
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
        auto op_prec = get_op_precedence(tok_type);
        if (op_prec < prec) {
            return op1;
        }
        //        if (op->type == TokenType::QUES) {
        //            parse_ternary_expr(x, x);
        //        } else {
        consume();
        auto op2 = parse_binary_expr(lhs, parent, op_prec + 1);
        auto node = create_node(NodeType::BinOpExpr, op_token);
        node->data.bin_op_expr.op1 = op1;
        node->data.bin_op_expr.op_type = tok_type;
        node->data.bin_op_expr.op2 = op2;
        op1 = node;
        //        }
    }
    return op1;
}

Node *Parser::parse_child_expr(bool lhs, Node *parent) {
    return parse_binary_expr(lhs, parent, DEFAULT_PREC);
}

Node *Parser::parse_unary_expr(bool lhs, Node *parent) {
    auto token = get();
    switch (token->type) {
    case TokenType::KW_NEW:
        return parse_construct_expr();

    case TokenType::KW_SIZEOF:
    case TokenType::KW_DELETE:
        return parse_prefix_expr();

    case TokenType::ADD:
    case TokenType::AND:
    case TokenType::MUL:
    case TokenType::SUB:
    case TokenType::LNOT:
    case TokenType::INC:
    case TokenType::DEC: {
        consume();
        auto node = create_unary_expr_node(token);

        if (token->type == TokenType::AND && get()->type == TokenType::KW_MUT) {
            consume();
            node->data.unary_op_expr.op_type = TokenType::MUTREF;
        }

        auto operand = parse_child_expr(lhs, parent);
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
        node->data.try_expr.expr = parse_child_expr(lhs, parent);
        return node;
    }

    case TokenType::KW_AWAIT: {
        consume();
        auto node = create_node(NodeType::AwaitExpr, token);
        node->data.await_expr.expr = parse_child_expr(lhs, parent);
        return node;
    }

    default:
        return parse_primary_expr(lhs, parent);
    }
}

Node *Parser::parse_primary_expr(bool lhs, Node *parent) {
    auto node = parse_operand(lhs, parent);
    for (;;) {
        auto token = get();
        switch (token->type) {
        case TokenType::DOT:
            node = parse_dot_expr(node);
            break;

        case TokenType::KW_AS: {
            auto expr = node;
            node = create_node(NodeType::CastExpr, token);
            consume();
            node->data.cast_expr.dest_type = parse_type_expr(true);
            node->data.cast_expr.expr = expr;
            break;
        }

        case TokenType::LBRACK:
            node = parse_index_expr(node);
            break;

        case TokenType::LT:
            // Check if this is a function call with type parameters
            if (is_function_call_with_type_params()) {
                node = parse_fn_call_with_type_params(node, lhs, parent);
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
    case TokenType::KW_NEW:
    case TokenType::KW_DELETE:
    case TokenType::IDEN: {
        return parse_identifier();
    }
    case TokenType::INT:
    case TokenType::BOOL:
    case TokenType::NULLP:
    case TokenType::KW_UNDEFINED:
    case TokenType::FLOAT:
    case TokenType::CHAR:
    case TokenType::STRING: {
        consume();
        return create_node(NodeType::LiteralExpr, token);
    }
    case TokenType::LPAREN: {
        consume();
        auto node = create_node(NodeType::ParenExpr, token);
        node->data.child_expr = parse_child_expr(lhs, parent);
        expect(TokenType::RPAREN);
        return node;
    }
    case TokenType::KW_FUNC: {
        return parse_fn_lambda();
    }
    case TokenType::KW_SWITCH: {
        return parse_switch_expr();
    }
    case TokenType::LBRACE: {
        return parse_construct_expr();
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
            auto arg = parse_child_expr_construct(lhs, parent);
            node->data.fn_call_expr.args.add(arg);
        }
        if (!at_comma(TokenType::RPAREN)) {
            break;
        }
        consume();
    }
    expect(TokenType::RPAREN);
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
    expect(TokenType::RPAREN);
    return node;
}

bool Parser::try_parse_fn_type_lookahead(int &pos) {
    // func might not have parentheses
    auto token = lookahead(pos);
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

    // Handle pointer sigil
    if (token->type == TokenType::MUL) {
        pos++;
        token = lookahead(pos);
        if (token->type == TokenType::END) {
            return false;
        }
    }

    // Handle reference sigil
    if (token->type == TokenType::AND) {
        pos++;
        token = lookahead(pos);
        if (token->type == TokenType::END) {
            return false;
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

            // Recursively parse type argument
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
    if (!ok) {
        error(node->token, errors::REDECLARED, name);
    }
}

Node *Parser::parse_if_stmt() {
    auto kw = expect(TokenType::KW_IF);
    auto node = create_node(NodeType::IfStmt, kw);
    auto scope = m_ctx->resolver->push_scope(node);
    node->data.if_stmt.condition = parse_expr();
    node->data.if_stmt.then_block = parse_block(scope);
    auto token = get();
    if (token->type == TokenType::KW_ELSE) {
        consume();
        token = get();
        if (token->type == TokenType::KW_IF) {
            node->data.if_stmt.else_node = parse_if_stmt();
        } else if (token->type == TokenType::LBRACE) {
            node->data.if_stmt.else_node = parse_block();
        } else {
            error(token, "expecting if statement or block");
        }
    }
    m_ctx->resolver->pop_scope();
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
                }

                is_range = true;
            }

            // Check the 'in' keyword for for in loop, or fallback to ternary
            if (is_range || lookahead(1)->type == TokenType::KW_IN) {
                auto iden = expect(TokenType::IDEN);
                auto bind = create_node(NodeType::BindIdentifier, iden);
                node->data.for_stmt.bind = bind;
                kind = ForLoopKind::Range;
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
            node->data.for_stmt.expr = parse_expr();
            auto bind = node->data.for_stmt.bind;
            if (node->data.for_stmt.bind->name != "_") {
                add_to_scope(bind, bind->name);
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

    if (next_is(TokenType::LPAREN)) {
        consume();
        auto iden = expect(TokenType::IDEN);
        node->data.enum_decl.discriminator_field = iden;
        expect(TokenType::COLON);
        auto type = parse_type_expr(true);
        node->data.enum_decl.discriminator_type = type;
        expect(TokenType::RPAREN);
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
            auto param_iden = expect(TokenType::IDEN);
            auto param_node = create_node(NodeType::TypeParam, param_iden);

            // Check for colon syntax for type bounds: T: SomeInterface
            if (next_is(TokenType::COLON)) {
                consume(); // consume the colon
                param_node->data.type_param.type_bound = parse_type_expr(true);
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
    if (next_is(TokenType::KW_IMPLEMENTS)) {
        save_block_pos(node);
        consume();
        while (get()->type != TokenType::LBRACE) {
            if (get()->type == TokenType::END) {
                error(get(), errors::UNEXPECTED_EOF);
                break;
            }
            consume();
        }
    } else {
        save_block_pos(node);
    }
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
        auto spec = parse_decl_spec();
        if (next_is(TokenType::KW_FUNC)) {
            return parse_fn_decl(FN_BODY_REQUIRED, spec);
        }
        return parse_var_decl(true, spec);
    }
}

void Parser::parse_enum_block(Node *node) {
    m_ctx->resolver->push_scope(node);
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
    if (next_is(TokenType::KW_IMPLEMENTS)) {
        consume();
        for (;;) {
            auto token = get();
            if (token->type == TokenType::END) {
                error(token, errors::UNEXPECTED_EOF);
                return;
            }
            if (token->type == TokenType::LBRACE) {
                break;
            }

            // Check if we have a valid interface type followed by proper syntax
            auto saved_pos = m_toki;
            auto expr = parse_type_expr(true);

            // If the next token after the type is not comma or LBRACE,
            // this is malformed (missing opening brace)
            auto next_token = get();
            if (next_token->type != TokenType::COMMA && next_token->type != TokenType::LBRACE) {
                // This is malformed - missing opening brace after implements
                error(next_token, "expected '{{' after implements clause, got '{}'",
                      next_token->to_string());
                node->data.struct_decl.implements.add(create_error_node());
                return; // Stop parsing implements and let caller handle the error
            }

            node->data.struct_decl.implements.add(expr);
            if (!at_comma(TokenType::LBRACE)) {
                break;
            }
            consume();
        }
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

        if (token->type == TokenType::DOT) {
            // field initializer
            field_started = true;
            consume();
            auto field = expect_identifier();
            expect(TokenType::ASS);
            auto value = parse_expr();
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

            auto expr = parse_expr();
            if (expr && expr->type != NodeType::Error) {
                node->data.construct_expr.items.add(expr);
            } else {
                // If parse_expr failed, consume token and continue
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
    expect(TokenType::RBRACE);
    return node;
}

Node *Parser::parse_prefix_expr() {
    auto node = create_node(NodeType::PrefixExpr, get());
    auto tok = get();
    consume();
    node->data.prefix_expr.prefix = tok;
    if (tok->type == TokenType::KW_SIZEOF) {
        node->data.prefix_expr.expr = parse_type_expr(true);
    } else {
        node->data.prefix_expr.expr = parse_expr();
    }
    return node;
}

Node *Parser::parse_dot_expr(Node *expr) {
    auto dot = expect(TokenType::DOT);
    auto node = create_node(NodeType::DotExpr, dot);
    node->data.dot_expr.expr = expr;
    node->data.dot_expr.field = expect_identifier();

    if (next_is(TokenType::DOT)) {
        return parse_dot_expr(node);
    }
    return node;
}

Node *Parser::parse_index_expr(Node *expr) {
    auto lb = expect(TokenType::LBRACK);
    auto node = create_node(NodeType::IndexExpr, lb);
    node->data.index_expr.expr = expr;
    node->data.index_expr.subscript = parse_expr();
    expect(TokenType::RBRACK);
    return node;
}

Node *Parser::parse_typedef() {
    auto token = expect(TokenType::KW_TYPEDEF);
    auto node = create_node(NodeType::TypedefDecl, token);
    auto iden = expect(TokenType::IDEN);
    node->data.typedef_decl.identifier = iden;
    expect(TokenType::ASS);
    node->data.typedef_decl.type = parse_type_expr(true);
    add_to_scope(node, iden->str);
    expect(TokenType::SEMICOLON);
    return node;
}

Node *Parser::parse_enum_member(Node *parent) {
    auto iden = expect(TokenType::IDEN);
    auto node = create_node(NodeType::EnumVariant, iden);
    node->data.enum_variant.name = iden;
    node->data.enum_variant.parent = parent;
    if (next_is(TokenType::LBRACE)) {
        consume();
        auto struct_node = create_node(NodeType::StructDecl, iden);
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

Node *Parser::parse_extern_decl() {
    auto kw = expect(TokenType::KW_EXTERN);
    auto type = expect(TokenType::STRING);

    auto node = create_node(NodeType::ExternDecl, kw);
    node->data.extern_decl = {};
    node->data.extern_decl.type = type;

    auto &members = node->data.extern_decl.members;
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
            break;
        }

        auto fn = parse_fn_decl(FN_BODY_NONE);
        fn->data.fn_def.decl_spec->flags |= DECL_EXTERN;
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
    node->data.import_decl.path = expect(TokenType::STRING);
    if (next_is(TokenType::KW_AS)) {
        consume();
        auto iden = expect(TokenType::IDEN);
        node->data.import_decl.alias = iden;
        add_to_scope(node, iden->str);
    } else if (next_is(TokenType::LBRACE)) {
        consume();
        while (!next_is(TokenType::RBRACE)) {
            auto iden = expect(TokenType::IDEN);
            auto member = create_node(NodeType::ImportSymbol, iden);
            member->data.import_symbol.name = iden;
            auto name_iden = iden;
            if (next_is(TokenType::KW_AS)) {
                consume();
                name_iden = expect(TokenType::IDEN);
                member->data.import_symbol.alias = name_iden;
            }
            if (!next_is(TokenType::RBRACE)) {
                expect(TokenType::COMMA);
            }
            node->data.import_decl.symbols.add(member);
            member->data.import_symbol.import = node;
            add_to_scope(member, name_iden->get_name());
        }
        expect(TokenType::RBRACE);
    }

    expect(TokenType::SEMICOLON);
    return node;
}

Node *Parser::parse_export_decl() {
    auto kw = expect(TokenType::KW_EXPORT);
    auto node = create_node(NodeType::ExportDecl, kw);
    node->data.export_decl = {};
    node->data.export_decl.path = expect(TokenType::STRING);
    node->data.export_decl.decl_spec = m_ctx->allocator->create_decl_spec();

    if (next_is(TokenType::KW_AS)) {
        consume();
        auto name_iden = expect(TokenType::IDEN);
        node->data.export_decl.alias = name_iden;
        node->name = name_iden->get_name();
    } else {
        if (next_is(TokenType::MUL)) {
            auto ellipsis = get();
            consume();
            node->data.export_decl.match_all = ellipsis;
        } else if (next_is(TokenType::LBRACE)) {
            consume();
            while (!next_is(TokenType::RBRACE)) {
                auto iden = expect(TokenType::IDEN);
                auto member = create_node(NodeType::ImportSymbol, iden);
                member->data.import_symbol.name = iden;
                auto name_iden = iden;
                if (next_is(TokenType::KW_AS)) {
                    consume();
                    name_iden = expect(TokenType::IDEN);
                    member->data.import_symbol.alias = name_iden;
                }
                if (!next_is(TokenType::RBRACE)) {
                    expect(TokenType::COMMA);
                }
                node->data.export_decl.symbols.add(member);
                member->data.import_symbol.import = node;
                add_to_scope(member, name_iden->get_name());
            }
            expect(TokenType::RBRACE);
        } else {
            error(get(), errors::EXPORT_DECL_MUST_HAVE_SYMBOLS);
        }
    }

    expect(TokenType::SEMICOLON);
    return node;
}

Node *Parser::parse_switch_expr() {
    auto kw = expect(TokenType::KW_SWITCH);
    auto node = create_node(NodeType::SwitchExpr, kw);
    auto scope = m_ctx->resolver->push_scope(node);

    node->data.switch_expr.expr = parse_expr();
    expect(TokenType::LBRACE);
    while (!next_is(TokenType::RBRACE)) {
        auto case_expr = parse_case_expr();
        node->data.switch_expr.cases.add(case_expr);
        if (!at_comma(TokenType::RBRACE)) {
            break;
        }
        consume();
    }
    bool has_else = false;
    for (auto case_expr : node->data.switch_expr.cases) {
        if (case_expr->data.case_expr.is_else) {
            has_else = true;
            break;
        }
    }
    if (!has_else) {
        error(kw, errors::SWITCH_EXPR_MUST_HAVE_ELSE);
    }
    expect(TokenType::RBRACE);
    m_ctx->resolver->pop_scope();
    return node;
}

Node *Parser::parse_case_expr() {
    Token *token = nullptr;
    Node *node = nullptr;
    if (next_is(TokenType::KW_ELSE)) {
        auto token = read();
        node = create_node(NodeType::CaseExpr, token);
        node->data.case_expr.is_else = true;

    } else {
        auto expr = parse_expr();
        node = create_node(NodeType::CaseExpr, expr->token);
        node->data.case_expr.clauses = {expr};
        if (next_is(TokenType::COMMA)) {
            consume();
            while (!next_is(TokenType::ARROW)) {
                auto expr = parse_expr();
                node->data.case_expr.clauses.add(expr);
                if (!at_comma(TokenType::ARROW)) {
                    break;
                }
                consume();
            }
        }
    }

    auto arrow = expect(TokenType::ARROW);
    node->data.case_expr.body = parse_block(nullptr, arrow);
    return node;
}