/*
 * Copyright (c) 2018 Hai Thanh Nguyen
 *
 * This file is part of chi, which is MIT licensed.
 * See http://opensource.org/licenses/MIT
 */

#pragma once

#include <deque>

#include "lexer.h"
#include "ast.h"
#include "resolver.h"

using namespace cx::ast;

namespace cx {
    struct ParseContext {
        Module* module;
        array<Token>* tokens;
        Allocator* allocator;
        ScopeResolver* resolver;
    };

    class Parser {
        ParseContext* m_ctx;
        size_t m_token_i = 0;
        Token m_eof_token;

        Token* next();

        Token* read();

        Token* get();

        Token* lookahead(int n);

        void skip_to(long offset);

        void unread();

        Token* expect(TokenType expected);

        void consume() { read(); }

        template<typename... Args>
        void error(Token* token, const char* format, const Args& ...args) {
            print("{}:{}:{}: error: {}\n", m_ctx->module->path, token->pos.line + 1,
                  token->pos.col + 1, fmt::format(format, args...));
            exit(0);
        }

    public:
        Parser(ParseContext* ctx) { m_ctx = ctx; }

        Node* create_node(NodeType type, Token*);

        Node* create_error_node();

        void unexpected(Token* token);

        bool at_comma(TokenType end_token);

        int get_op_precedence(TokenType op_type);

        bool next_is_type_expr();

        void add_to_scope(Node* node);

        void parse();

        Node* parse_root();

        void parse_top_level_decls(NodeList* decls);

        Node* parse_top_level_decl();

        Node* parse_var_or_fn_decl();

        Node* parse_identifier();

        IdentifierKind get_identifier_kind(Node* node);

        Node* parse_type_expr();

        Node* parse_fn_decl(Node* return_type, Token* iden);

        Node* parse_var_decl(Node* type_expr, Token* iden);

        Node* parse_fn_proto(Node* return_type, Token* iden);

        void parse_fn_params(NodeList* params);

        Node* parse_fn_param();

        Node* parse_block();

        Node* parse_stmt();

        Node* parse_expr();

        Node* parse_expr_clause(bool lhs);

        Node* parse_binary_expr(bool lhs, Node* parent, int prec);

        Node* parse_unary_expr(bool lhs, Node* parent);

        Node* parse_primary_expr(bool lhs, Node* parent);

        Node* parse_operand(bool lhs, Node* parent);

        Node* parse_fn_call_expr(Node* fn_expr, bool lhs, Node* parent);

        Node* parse_simple_stmt();

        Node* parse_return_stmt();

        Node* parse_if_stmt();
    };
}
