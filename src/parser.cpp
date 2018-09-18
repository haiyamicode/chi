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
const int COMMA_PREC = 0;
const int TERNARY_PREC = 1;

void cx::Parser::parse() {
    parse_root(m_ctx->module->root);
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

void Parser::reset_buffers() {
    m_lookahead.clear();
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
    m_bufi = (m_bufi + 1) % BUF_LEN;
    if (!m_lookahead.empty()) {
        m_buf[m_bufi] = m_lookahead.front();
        m_lookahead.pop_front();
    } else {
        m_buf[m_bufi] = next();
    }
    return m_buf[m_bufi];
}

void Parser::skip_to(long offset) {
    Token* token;
    do {
        token = read();
    } while (token->type != TokenType::END && token->pos.offset < offset);
    unread();
}

void Parser::unread() {
    m_lookahead.push_front(m_buf[m_bufi]);
    m_bufi = (m_bufi + BUF_LEN - 1) % BUF_LEN; // circularly decreases the index
}

Token* Parser::peek() { return lookahead(1); }

Token* Parser::lookahead(int n) {
    for (int m = int(m_lookahead.size()); m < n; m++) {
        m_lookahead.push_back(next());
    }
    return m_lookahead[n - 1];
}

bool Parser::at_comma(TokenType end_token) {
    auto token = peek();
    if (token->type == TokenType::COMMA) {
        return true;
    } else if (token->type == end_token) {
        return false;
    }
    unexpected(token);
    return true;
}

Node* Parser::create_node(NodeType type, Token* token) {
    auto node = (Node*) m_ctx->alloc(sizeof(Node));
    node->type = type;
    node->token = token;
    return node;
}

Node* Parser::create_error_node() {
    return create_node(NodeType::Error, read());
}

void Parser::parse_root(Node* root) {
    auto node = create_node(NodeType::Root, peek());
    parse_top_level_decls(&node->data.root.top_level_decls);
}

void Parser::parse_top_level_decls(NodeList* decls) {
    reset_buffers();
    for (;;) {
        if (peek()->type == TokenType::END) {
            break;
        }
        decls->add(parse_top_level_decl());
    }
}

Node* Parser::parse_top_level_decl() {
    auto node = parse_var_or_func_decl();
    m_ctx->resolver->declare_symbol(node->name, node);
    return node;
}

Node* Parser::parse_var_or_func_decl() {
    auto type_expr = parse_type_expr();
    auto iden = expect(TokenType::IDEN);
    auto token = peek();
    if (token->type == TokenType::LPAREN) {
        consume();
        return parse_func_decl(type_expr, iden);
    } else {
        return parse_var_decl(type_expr, iden);
    }
}

Node* Parser::parse_type_expr() {
    auto token = expect(TokenType::IDEN);
    auto node = create_node(NodeType::Identifier, token);
    node->name = token->str;
    return node;
}

Node* Parser::parse_func_decl(Node* return_type, Token* iden) {
    auto fn = create_node(NodeType::FnDef, return_type->token);
    fn->data.fn_def.fn_proto = parse_func_proto(return_type, iden);
    fn->data.fn_def.body = parse_block();
    return fn;
}

Node* Parser::parse_var_decl(Node* type_expr, Token* iden) {
    auto node = create_node(NodeType::VarDecl, type_expr->token);
    node->data.var_decl.type = type_expr;
    auto token = peek();
    if (token->type == TokenType::ASS) {
        consume();
        node->data.var_decl.expr = parse_expr_clause(false);
    }
    expect(TokenType::SEMICOLON);
    return node;
}

Node* Parser::parse_func_proto(Node* return_type, Token* iden) {
    auto proto = create_node(NodeType::FnProto, return_type->token);
    proto->name = iden->str;
    proto->data.fn_proto.return_type = return_type;
    parse_func_params(&proto->data.fn_proto.params);
    return proto;
}

void Parser::parse_func_params(NodeList* params) {
    Token* token;
    for (;;) {
        token = peek();
        if (token->type == TokenType::RPAREN) {
            break;
        }
        params->add(parse_func_param());
        if (!at_comma(TokenType::RPAREN)) {
            break;
        }
        consume();
    }
    expect(TokenType::RPAREN);
}

Node* Parser::parse_func_param() {
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
        auto token = peek();
        if (token->type == TokenType::RBRACE) {
            consume();
            return node;
        }
        node->data.block.statements.add(parse_stmt());
    }
}

Node* Parser::parse_stmt() {
    auto token = peek();
    switch (token->type) {
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
    return parse_expr();
}

bool Parser::next_is_type_expr() {
    auto token = peek();
    if (auto node = m_ctx->resolver->find_symbol(token->str)) {
        if (node->type == NodeType::Identifier && node->data.identifier.kind == IdentifierKind::TypeName) {
            return true;
        }
    }
    return false;
}

Node* Parser::parse_expr() {
    auto lhs = parse_expr_clause(true);
    auto token = peek();
    switch (token->type) {
        case TokenType::SEMICOLON:
            return lhs;

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
            unexpected(token);
            return create_error_node();
    }
    auto node = create_node(NodeType::BinOpExpr, lhs->token);
    node->data.bin_op_expr.op1 = lhs;
    node->data.bin_op_expr.op_type = token->type;
    node->data.bin_op_expr.op2 = parse_expr_clause(false);
    return node;
}

Node* Parser::parse_expr_clause(bool lhs) {
    return parse_binary_expr(lhs, nullptr, 0);
}

Node* Parser::parse_binary_expr(bool lhs, Node* parent, int prec) {
    auto op1 = parse_unary_expr(lhs, parent);
    for (;;) {
        auto op = peek();
        auto op_prec = get_op_precedence(op->type);
        if (op_prec < prec) {
            return op1;
        }
//        if (op->type == TokenType::QUES) {
//            parse_ternary_expr(x, x);
//        } else {
        consume();
        auto op2 = parse_binary_expr(lhs, parent, op_prec + 1);
        auto node = create_node(NodeType::BinOpExpr, op1->token);
        node->data.bin_op_expr.op1 = op1;
        node->data.bin_op_expr.op_type = op->type;
        node->data.bin_op_expr.op2 = op2;
        return node;
//        }
    }
}

Node* Parser::parse_unary_expr(bool lhs, Node* parent) {
    return parse_primary_expr(lhs, parent);
}

Node* Parser::parse_primary_expr(bool lhs, Node* parent) {
    auto node = parse_operand(lhs, parent);
    for (;;) {
        auto token = peek();
        switch (token->type) {
//            case TokenType::DOT:
//                break;
//            case TokenType::LBRACK: {
//                break;

            case TokenType::LPAREN:
                node = parse_func_call_expr(node, lhs, parent);

            default:
                return node;
        }
    }
}

Node* Parser::parse_operand(bool lhs, Node* parent) {
    auto token = peek();
    Node* node = create_node(NodeType::Error, token);
    switch (token->type) {
        case TokenType::IDEN:
            consume();
            node->type = NodeType::Identifier;
            node->name = token->str;
            break;
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
            node->data.child_expr = parse_expr();
            expect(TokenType::RPAREN);
            break;

        default:
            unexpected(token);
    }
    return create_error_node();
}

Node* Parser::parse_func_call_expr(Node* fn_expr, bool lhs, Node* parent) {
    auto node = create_node(NodeType::FnCallExpr, fn_expr->token);
    expect(TokenType::LPAREN);

    for (;;) {
        auto tok = peek();
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
    return node;
}
