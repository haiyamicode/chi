/*
 * Copyright (c) 2018 Hai Thanh Nguyen
 *
 * This file is part of chi, which is MIT licensed.
 * See http://opensource.org/licenses/MIT
 */

#include "parser.h"
#include "ast.h"
#include "errors.h"

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
        case TokenType::INT:
            return "int literal";
        case TokenType::FLOAT:
            return "float literal";
        default:
            return fmt::format("'{}'", get_token_symbol(token_type));
    }
}

Parser::Parser(ParseContext* ctx) {
    m_ctx = ctx;
}

void Parser::parse() {
    m_ctx->module->root = parse_root();
}

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

Token* Parser::expect(TokenType expected) {
    auto token = read();
    if (token->type == expected) {
        return token;
    } else {
        unread();
        expected_got(expected, token);
        return token;
    }
}

void Parser::expected_got(TokenType expected, Token* token) {
    auto expected_str = get_token_type_repr(expected);
    error(token, errors::TOKEN_EXPECTED_GOT, expected_str, token->to_string());
}

void Parser::unexpected(Token* token) {
    error(token, errors::TOKEN_UNEXPECTED, token->to_string());
}

Token* Parser::next() {
    if (m_toki >= m_ctx->tokens->size) {
        return &m_eof_token;
    }
    return &m_ctx->tokens->at(m_toki++);
}

Token* Parser::read() {
    return next();
}

void Parser::jump_to(size_t pos) {
    m_toki = pos;
}

void Parser::unread() {
    m_toki--;
}

Token* Parser::get() { return lookahead(0); }

Token* Parser::lookahead(int n) {
    if (m_toki + n >= m_ctx->tokens->size) {
        return &m_eof_token;
    }
    return &m_ctx->tokens->at(m_toki + n);
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

Node* Parser::create_node(NodeType type, Token* token) {
    auto node = m_ctx->allocator->create_node(type);
    node->token = token;
    node->module = m_ctx->module;
    if (token->type == TokenType::IDEN) {
        node->name = token->str;
    }
    return node;
}

Node* Parser::create_identifier_node(Token* iden, Node* decl) {
    auto node = create_node(NodeType::Identifier, iden);
    node->data.identifier.decl = decl;
    if (decl) {
        node->data.identifier.kind = get_identifier_kind(decl);;
    }
    return node;
}

Node* Parser::create_error_node() {
    return create_node(NodeType::Error, read());
}

Node* Parser::parse_root() {
    auto node = create_node(NodeType::Root, get());
    this->m_ctx->module->root = node;
    parse_top_level_decls(&node->data.root.top_level_decls);
    return node;
}

void Parser::parse_top_level_decls(NodeList* decls) {
    // first pass, skip all function and struct blocks
    for (;;) {
        auto token = get();
        if (token->type == TokenType::END) {
            break;
        }
        if (token->type == TokenType::KW_EXTERN || token->type == TokenType::KW_INLINE ||
            token->type == TokenType::KW_STATIC) {
            consume();
        }
        decls->add(parse_top_level_decl());
    }
    // second pass, parse function and struct blocks
    for (auto decl: *decls) {
        if (decl->type == NodeType::FnDef) {
            jump_to(m_block_pos[decl]);
            parse_fn_block(decl);
        } else if (decl->type == NodeType::StructDecl) {
            jump_to(m_block_pos[decl]);
            parse_struct_block(decl);
        }
    }
}

Node* Parser::parse_top_level_decl() {
    auto token = get();
    switch (token->type) {
        case TokenType::KW_STRUCT:
        case TokenType::KW_UNION:
        case TokenType::KW_ENUM:
        case TokenType::KW_TRAIT:
            return parse_struct_decl(token->type);
        case TokenType::KW_TYPEDEF:
            return parse_typedef();
        default:
            return parse_var_or_fn_decl();
    }
}

optional<FnKind> Parser::parse_decl_identifier(Token** iden) {
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
            fn_kind = FnKind::InstanceMethod;
            *iden = expect(TokenType::IDEN);
        }
    } else {
        *iden = expect(TokenType::IDEN);
    }
    if (get()->type != TokenType::LPAREN) {
        return {};
    }
    return fn_kind;
}

Node* Parser::parse_var_or_fn_decl(bool requires_body) {
    auto type_expr = parse_type_expr();
    Token* iden = nullptr;
    auto fn_kind = parse_decl_identifier(&iden);
    if (fn_kind) {
        return parse_fn_decl(type_expr, iden, *fn_kind, requires_body);
    } else {
        unread();
        return parse_var_decl(type_expr);
    }
}

IdentifierKind Parser::get_identifier_kind(Node* node) {
    if (node->type == NodeType::Primitive
        || node->type == NodeType::StructDecl
        || node->type == NodeType::TypedefDecl) {
        return IdentifierKind::TypeName;
    } else {
        return IdentifierKind::Value;
    }
}

Node* Parser::parse_identifier() {
    auto token = expect(TokenType::IDEN);
    auto decl = m_ctx->resolver->find_symbol(token->str);
    auto node = create_identifier_node(token, decl);
    if (!decl) {
        error(token, errors::UNDECLARED, node->name);
    }
    return node;
}

Node* Parser::create_type_sigil_node(Node* type, SigilKind sigil) {
    auto node = create_node(NodeType::TypeSigil, type->token);
    node->data.type_sigil.type = type;
    node->data.type_sigil.sigil = sigil;
    return node;
}

Node* Parser::parse_type_expr() {
    auto iden = parse_identifier();
    auto type = iden;
    for (;;) {
        switch (get()->type) {
            case TokenType::MUL:
                type = create_type_sigil_node(type, SigilKind::Pointer);
                type->token = read();
                continue;
            default:
                break;
        }
        break;
    }
    if (next_is(TokenType::LT)) {
        consume();
        auto node = create_node(NodeType::SubtypeExpr, iden->token);
        auto& subtype = node->data.subtype_expr;
        subtype.type = type;
        Token* token;
        for (;;) {
            token = get();
            if (token->type == TokenType::GT) {
                break;
            }
            auto param = parse_type_expr();
            subtype.args.add(param);
            if (!at_comma(TokenType::GT)) {
                break;
            }
            consume();
        }
        expect(TokenType::GT);
        return node;
    }
    return type;
}

Node* Parser::parse_fn_decl(Node* return_type, Token* iden, FnKind kind, bool requires_body) {
    expect(TokenType::LPAREN);
    auto fn = create_node(NodeType::FnDef, iden);
    fn->name = iden->str;
    auto proto = parse_fn_proto(return_type, iden);
    fn->data.fn_def.fn_proto = proto;
    fn->data.fn_def.fn_kind = kind;
    fn->data.fn_def.container = get_scope()->owner;
    proto->data.fn_proto.fn_def_node = fn;
    if (kind == FnKind::TopLevel) {
        save_block_pos(fn);
        skip_block();
    } else {
        if (!requires_body && next_is(TokenType::SEMICOLON)) {
            consume();
        } else {
            parse_fn_block(fn);
        }
    }
    add_to_scope(fn);
    return fn;
}

void Parser::parse_fn_block(Node* fn) {
    m_ctx->resolver->push_scope(fn);
    auto& fn_def = fn->data.fn_def;
    auto& fn_proto = fn_def.fn_proto->data.fn_proto;
    for (auto param: fn_proto.params) {
        add_to_scope(param);
    }
    fn_def.body = parse_block();
    m_ctx->resolver->pop_scope();
}

Node* Parser::parse_var_identifier() {
    auto token = expect(TokenType::IDEN);
    auto node = create_node(NodeType::VarIdentifier, token);
    if (next_is(TokenType::LBRACK)) {
        consume();
        node->data.var_identifier.size_expr = parse_expr_clause(false);
        expect(TokenType::RBRACK);
    }
    return node;
}

Node* Parser::parse_var_decl(Node* type_expr) {
    auto iden = parse_var_identifier();
    auto node = create_node(NodeType::VarDecl, iden->token);
    node->data.var_decl.type = type_expr;
    node->data.var_decl.identifier = iden;
    if (next_is(TokenType::ELLIPSIS)) {
        consume();
        node->data.var_decl.is_embed = true;
    }
    if (next_is(TokenType::ASS)) {
        consume();
        node->data.var_decl.expr = parse_expr_clause(false);
    }
    expect(TokenType::SEMICOLON);
    add_to_scope(node);
    return node;
}

Node* Parser::parse_fn_proto(Node* return_type, Token* iden) {
    auto proto = create_node(NodeType::FnProto, iden);
    proto->name = iden->str;
    proto->data.fn_proto.return_type = return_type;
    parse_fn_params(&proto->data.fn_proto.params);
    return proto;
}

void Parser::parse_fn_params(NodeList* params) {
    Token* token;
    for (;;) {
        token = get();
        if (token->type == TokenType::RPAREN) {
            break;
        }
        auto param = parse_fn_param();
        params->add(param);
        if (!at_comma(TokenType::RPAREN)) {
            break;
        }
        consume();
    }
    expect(TokenType::RPAREN);
}

Node* Parser::parse_fn_param() {
    Token* token = get();
    optional<string> name;
    auto type = parse_type_expr();
    if (next_is(TokenType::IDEN)) {
        token = expect(TokenType::IDEN);
        name = token->str;
    }
    auto param = create_node(NodeType::ParamDecl, token);
    param->data.param_decl.type = type;
    if (name) {
        param->name = *name;
    }
    return param;
}

Node* Parser::parse_block() {
    auto lb = expect(TokenType::LBRACE);
    auto node = create_node(NodeType::Block, lb);
    for (;;) {
        auto token = get();
        if (token->type == TokenType::RBRACE) {
            consume();
            return node;
        }
        auto stmt = parse_stmt();
        node->data.block.statements.add(stmt);
    }
}

Node* Parser::parse_stmt() {
    auto token = get();
    switch (token->type) {
        case TokenType::KW_IF:
            return parse_if_stmt();

        case TokenType::KW_FOR:
            return parse_for_stmt();

        case TokenType::KW_LET:
        case TokenType::KW_THIS:
        case TokenType::IDEN:
        case TokenType::BOOL:
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
            return parse_simple_stmt();

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
            return create_error_node();
    }
}

Node* Parser::parse_simple_stmt(bool semicolon) {
    if (next_is_type_expr()) {
        auto type_expr = parse_type_expr();
        return parse_var_decl(type_expr);
    }
    auto node = parse_expr();
    if (semicolon) {
        expect(TokenType::SEMICOLON);
    }
    return node;
}

bool Parser::next_is_type_expr() {
    auto token = get();
    if (auto node = m_ctx->resolver->find_symbol(token->str)) {
        if (get_identifier_kind(node) == IdentifierKind::TypeName) {
            return true;
        }
    }
    return false;
}

bool Parser::next_is(TokenType token_type) {
    return get()->type == token_type;
}

Node* Parser::parse_expr() {
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
    auto node = create_node(NodeType::BinOpExpr, lhs->token);
    node->data.bin_op_expr.op1 = lhs;
    node->data.bin_op_expr.op_type = token->type;
    node->data.bin_op_expr.op2 = parse_expr_clause(false);
    return node;
}

Node* Parser::parse_expr_clause(bool lhs) {
    return parse_binary_expr(lhs, nullptr, DEFAULT_PREC);
}

Node* Parser::parse_binary_expr(bool lhs, Node* parent, int prec) {
    auto op1 = parse_unary_expr(lhs, parent);
    for (;;) {
        auto op_token = get();
        auto op_prec = get_op_precedence(op_token->type);
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
        node->data.bin_op_expr.op_type = op_token->type;
        node->data.bin_op_expr.op2 = op2;
        op1 = node;
//        }
    }
    return op1;
}

Node* Parser::parse_child_expr(bool lhs, Node* parent) {
    return parse_binary_expr(lhs, parent, DEFAULT_PREC);
}

Node* Parser::parse_unary_expr(bool lhs, Node* parent) {
    auto token = get();
    switch (token->type) {
        case TokenType::MUL:
        case TokenType::AND:
        case TokenType::SUB:
        case TokenType::ADD:
        case TokenType::LNOT:
        case TokenType::INC:
        case TokenType::DEC: {
            consume();
            auto node = create_unary_expr_node(token);
            node->data.unary_op_expr.op1 = parse_child_expr(lhs, parent);
            return node;
        }

        case TokenType::LPAREN: {
            consume();
            if (next_is_type_expr()) {
                auto node = create_node(NodeType::CastExpr, token);
                node->data.cast_expr.dest_type = parse_type_expr();
                expect(TokenType::RPAREN);
                node->data.cast_expr.expr = parse_child_expr(lhs, parent);
                return node;
            } else {
                unread();
            }
        }

        default:
            return parse_primary_expr(lhs, parent);
    }
}

Node* Parser::parse_primary_expr(bool lhs, Node* parent) {
    auto node = parse_operand(lhs, parent);
    for (;;) {
        auto token = get();
        switch (token->type) {
            case TokenType::DOT:
                node = parse_dot_expr(node);
                break;

            case TokenType::LBRACK:
                node = parse_index_expr(node);
                break;

            case TokenType::LPAREN:
                node = parse_fn_call_expr(node, lhs, parent);
                break;

            case TokenType::INC:
            case TokenType::DEC: {
                auto op1 = node;
                node = create_unary_expr_node(read());
                node->data.unary_op_expr.op1 = op1;
                node->data.unary_op_expr.is_suffix = true;
                return node;
            }

            default:
                return node;
        }
    }
}

Node* Parser::parse_operand(bool lhs, Node* parent) {
    auto token = get();
    switch (token->type) {
        case TokenType::KW_THIS: {
            consume();
            auto node = create_node(NodeType::Identifier, token);
            node->data.identifier.kind = IdentifierKind::This;
            node->name = "this";
            return node;
        }
        case TokenType::IDEN: {
            return parse_identifier();
        }
        case TokenType::INT:
        case TokenType::BOOL:
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
        case TokenType::LBRACE: {
            return parse_complit_expr();
        }
        default:
            unexpected(token);
    }
    return create_error_node();
}

Node* Parser::parse_fn_call_expr(Node* fn_expr, bool lhs, Node* parent) {
    auto node = create_node(NodeType::FnCallExpr, fn_expr->token);
    node->data.fn_call_expr.fn_ref_expr = fn_expr;
    expect(TokenType::LPAREN);

    for (;;) {
        auto tok = get();
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

Node* Parser::parse_return_stmt() {
    auto token = expect(TokenType::KW_RETURN);
    auto node = create_node(NodeType::ReturnStmt, token);
    if (get()->type != TokenType::SEMICOLON) {
        node->data.return_stmt.expr = parse_expr();
    }
    expect(TokenType::SEMICOLON);
    return node;
}

Node* Parser::parse_branch_stmt() {
    auto node = create_node(NodeType::BranchStmt, read());
    expect(TokenType::SEMICOLON);
    return node;
}

void Parser::add_to_scope(Node* node) {
    assert(!node->name.empty());
    add_to_scope(node, node->name);
}

void Parser::add_to_scope(Node* node, const string& name) {
    auto ok = m_ctx->resolver->declare_symbol(name, node);
    if (!ok) {
        error(node->token, errors::REDECLARED, name);
    }
}

Node* Parser::parse_if_stmt() {
    auto kw = expect(TokenType::KW_IF);
    auto node = create_node(NodeType::IfStmt, kw);
    m_ctx->resolver->push_scope(node);
    node->data.if_stmt.condition = parse_expr();
    node->data.if_stmt.then_block = parse_block();
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

Node* Parser::parse_for_stmt() {
    auto kw = expect(TokenType::KW_FOR);
    auto node = create_node(NodeType::ForStmt, kw);
    m_ctx->resolver->push_scope(node);
    if (!next_is(TokenType::LBRACE)) {
        expect(TokenType::LPAREN);
        if (next_is(TokenType::SEMICOLON)) {
            consume();
        } else {
            node->data.for_stmt.init = parse_simple_stmt();
        }
        if (!next_is(TokenType::SEMICOLON)) {
            node->data.for_stmt.condition = parse_expr();
        }
        expect(TokenType::SEMICOLON);
        if (!next_is(TokenType::RPAREN)) {
            node->data.for_stmt.post = parse_expr();
        }
        expect(TokenType::RPAREN);
    }
    node->data.for_stmt.body = parse_block();
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
        case TokenType::KW_TRAIT:
            return ContainerKind::Trait;
        default:
            return ContainerKind::Struct;
    }
}

Node* Parser::create_struct_node(Token* keyword, const string& name) {
    auto node = create_node(NodeType::StructDecl, keyword);
    node->data.struct_decl.kind = get_container_kind(keyword->type);
    node->name = name;
    return node;
}

Node* Parser::parse_struct_decl(TokenType keyword) {
    auto kw = expect(keyword);
    auto iden = expect(TokenType::IDEN);
    Node* node = create_struct_node(kw, iden->str);
    save_block_pos(node);
    skip_block();
    if (next_is(TokenType::SEMICOLON)) {
        consume();
    }
    add_to_scope(node);
    return node;
}

Node* Parser::parse_struct_member(ContainerKind container_kind) {
    switch (container_kind) {
        case ContainerKind::Enum:
            return parse_enum_member();
        case ContainerKind::Trait: {
            auto node = parse_var_or_fn_decl(false);
            if (node->type == NodeType::VarDecl) {
                error(node->token, errors::TRAIT_FIELD_NOT_ALLOWED);
            }
            return node;
        }
        default:
            return parse_var_or_fn_decl();
    }
}

void Parser::parse_struct_block(Node* node) {
    expect(TokenType::LBRACE);
    m_ctx->resolver->push_scope(node);
    while (get()->type != TokenType::RBRACE) {
        auto member = parse_struct_member(node->data.struct_decl.kind);
        node->data.struct_decl.members.add(member);
    }
    m_ctx->resolver->pop_scope();
    expect(TokenType::RBRACE);
}

Node* Parser::parse_complit_expr() {
    auto lb = expect(TokenType::LBRACE);
    auto node = create_node(NodeType::ComplitExpr, lb);
    Token* token;
    for (;;) {
        token = get();
        if (token->type == TokenType::RBRACE) {
            break;
        }
        auto expr = parse_expr();
        node->data.complit_expr.items.add(expr);
        if (!at_comma(TokenType::RBRACE)) {
            break;
        }
        consume();
    }
    expect(TokenType::RBRACE);
    return node;
}

Node* Parser::parse_dot_expr(Node* expr) {
    auto dot = expect(TokenType::DOT);
    auto node = create_node(NodeType::DotExpr, dot);
    node->data.dot_expr.expr = expr;
    node->data.dot_expr.field = expect(TokenType::IDEN);
    return node;
}

Node* Parser::parse_index_expr(Node* expr) {
    auto lb = expect(TokenType::LBRACK);
    auto node = create_node(NodeType::IndexExpr, lb);
    node->data.index_expr.expr = expr;
    node->data.index_expr.subscript = parse_expr();
    expect(TokenType::RBRACK);
    return node;
}

Node* Parser::parse_typedef() {
    auto token = expect(TokenType::KW_TYPEDEF);
    auto node = create_node(NodeType::TypedefDecl, token);
    node->data.typedef_decl.type = parse_type_expr();
    auto iden = parse_var_identifier();
    node->data.typedef_decl.identifier = iden;
    add_to_scope(node, iden->name);
    expect(TokenType::SEMICOLON);
    return node;
}

Node* Parser::parse_enum_member() {
    auto iden = expect(TokenType::IDEN);
    auto node = create_node(NodeType::EnumMember, iden);
    if (next_is(TokenType::ASS)) {
        consume();
        node->data.enum_member.value = parse_expr_clause(false);
    }
    if (!next_is(TokenType::RBRACE)) {
        expect(TokenType::COMMA);
    }
    return node;
}

Node* Parser::create_unary_expr_node(Token* token) {
    auto node = create_node(NodeType::UnaryOpExpr, token);
    node->data.unary_op_expr.op_type = token->type;
    return node;
}
