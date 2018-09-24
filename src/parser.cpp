/*
 * Copyright (c) 2018 Hai Thanh Nguyen
 *
 * This file is part of chi, which is MIT licensed.
 * See http://opensource.org/licenses/MIT
 */

#include "parser.h"
#include "ast.h"
#include "error.h"

using namespace cx;
using namespace cx::ast;

const int LOWEST_PREC = -1;
const int DEFAULT_PREC = LOWEST_PREC + 1;
const int COMMA_PREC = 0;
const int TERNARY_PREC = 1;

void cx::Parser::parse() {
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
        auto expected_str = Token(expected).to_string();
        auto got_str = token->to_string();
        error(token, errors::TOKEN_UNEXPECTED_GOT, expected_str, got_str);
        return token;
    }
}

void Parser::unexpected(Token* token) {
    error(token, errors::TOKEN_UNEXPECTED, token->to_string());
}

Token* Parser::next() {
    if (m_token_i >= m_ctx->tokens->size) {
        return &m_eof_token;
    }
    return &m_ctx->tokens->at(m_token_i++);
}

Token* Parser::read() {
    return next();
}

void Parser::jump_to(long pos) {
    m_token_i = pos;
}

void Parser::unread() {
    m_token_i--;
}

Token* Parser::get() { return lookahead(0); }

Token* Parser::lookahead(int n) {
    if (m_token_i + n >= m_ctx->tokens->size) {
        return &m_eof_token;
    }
    return &m_ctx->tokens->at(m_token_i + n);
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
    // first pass, skip all function bodies
    for (;;) {
        if (get()->type == TokenType::END) {
            break;
        }
        decls->add(parse_top_level_decl());
    }
    // second pass, parse function bodies
    for (auto decl: *decls) {
        if (decl->type == NodeType::FnDef) {
            parse_fn_body(decl);
        }
    }
}

Node* Parser::parse_top_level_decl() {
    return parse_var_or_fn_decl();
}

Node* Parser::parse_var_or_fn_decl() {
    auto type_expr = parse_type_expr();
    auto iden = expect(TokenType::IDEN);
    auto token = get();
    if (token->type == TokenType::LPAREN) {
        consume();
        return parse_fn_decl(type_expr, iden);
    } else {
        return parse_var_decl(type_expr, iden);
    }
}

IdentifierKind Parser::get_identifier_kind(Node* node) {
    if (node->type == NodeType::Primitive) {
        return IdentifierKind::TypeName;
    } else {
        return IdentifierKind::Value;
    }
}

Node* Parser::parse_identifier() {
    auto token = expect(TokenType::IDEN);
    auto node = create_node(NodeType::Identifier, token);
    node->name = token->str;
    auto& data = node->data.identifier;
    data.decl = m_ctx->resolver->find_symbol(node->name);
    if (!data.decl) {
        error(token, errors::UNDECLARED, node->name);
    } else {
        data.kind = get_identifier_kind(data.decl);
    }
    return node;
}

Node* Parser::parse_type_expr() {
    return parse_identifier();
}

Node* Parser::parse_fn_decl(Node* return_type, Token* iden) {
    auto fn = create_node(NodeType::FnDef, iden);
    fn->name = iden->str;
    auto proto = parse_fn_proto(return_type, iden);
    fn->data.fn_def.fn_proto = proto;
    proto->data.fn_proto.fn_def_node = fn;
    save_block_pos(fn);
    parse_block_skip();
    add_to_scope(fn);
    return fn;
}

void Parser::parse_fn_body(Node* fn) {
    auto pos = m_block_pos[fn];
    jump_to(pos);
    auto scope = m_ctx->resolver->push_scope();
    scope->owner = fn;
    auto& fn_def = fn->data.fn_def;
    auto& fn_proto = fn_def.fn_proto->data.fn_proto;
    for (auto param: fn_proto.params) {
        add_to_scope(param);
    }
    fn_def.body = parse_block();
    m_ctx->resolver->pop_scope();
}

Node* Parser::parse_var_decl(Node* type_expr, Token* iden) {
    auto node = create_node(NodeType::VarDecl, iden);
    node->name = iden->str;
    node->data.var_decl.type = type_expr;
    auto token = get();
    if (token->type == TokenType::ASS) {
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
    auto type = parse_type_expr();
    auto iden = expect(TokenType::IDEN);
    auto param = create_node(NodeType::ParamDecl, iden);
    param->data.param_decl.type = type;
    param->name = iden->str;
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

        case TokenType::KW_LET:
        case TokenType::IDEN:
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

Node* Parser::parse_simple_stmt() {
    if (next_is_type_expr()) {
        auto type_expr = parse_type_expr();
        return parse_var_decl(type_expr, expect(TokenType::IDEN));
    }
    auto node = parse_expr();
    expect(TokenType::SEMICOLON);
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

Node* Parser::parse_unary_expr(bool lhs, Node* parent) {
    return parse_primary_expr(lhs, parent);
}

Node* Parser::parse_primary_expr(bool lhs, Node* parent) {
    auto node = parse_operand(lhs, parent);
    for (;;) {
        auto token = get();
        switch (token->type) {
//            case TokenType::DOT:
//                break;
//            case TokenType::LBRACK: {
//                break;

            case TokenType::LPAREN:
                node = parse_fn_call_expr(node, lhs, parent);

            default:
                return node;
        }
    }
}

Node* Parser::parse_operand(bool lhs, Node* parent) {
    auto token = get();
    if (token->type == TokenType::IDEN) {
        return parse_identifier();
    }
    Node* node = create_node(NodeType::Error, token);
    switch (token->type) {
        case TokenType::INT:
        case TokenType::FLOAT:
        case TokenType::CHAR:
        case TokenType::STRING:
            consume();
            node->type = NodeType::LiteralExpr;
            break;

        case TokenType::LPAREN:
            consume();
            node->type = NodeType::ParenExpr;
            node->data.child_expr = parse_binary_expr(lhs, parent, DEFAULT_PREC);
            expect(TokenType::RPAREN);
            break;

        default:
            unexpected(token);
    }
    return node ? node : create_error_node();
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
            auto arg = parse_binary_expr(lhs, parent, COMMA_PREC + 1);
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
    node->data.return_stmt.expr = parse_expr();
    expect(TokenType::SEMICOLON);
    return node;
}

void Parser::add_to_scope(Node* node) {
    assert(!node->name.empty());
    auto ok = m_ctx->resolver->declare_symbol(node->name, node);
    if (!ok) {
        error(node->token, errors::REDECLARED, node->name);
    }
}

Node* Parser::parse_if_stmt() {
    auto kw = expect(TokenType::KW_IF);
    auto node = create_node(NodeType::IfStmt, kw);
    auto scope = m_ctx->resolver->push_scope();
    scope->owner = node;
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

void Parser::parse_block_skip() {
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
