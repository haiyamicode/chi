/*
 * Copyright (c) 2018 Hai Thanh Nguyen
 *
 * This file is part of chi, which is MIT licensed.
 * See http://opensource.org/licenses/MIT
 */

#pragma once

#include <deque>

#include "ast.h"
#include "lexer.h"
#include "resolver.h"

using namespace cx::ast;

namespace cx {
struct ParseContext {
    Module *module;
    Context *allocator;
    ScopeResolver *resolver;
    bool debug_mode = false;
    optional<ErrorHandler> error_handler = {};
    array<Token *> tokens;

    void add_token_results(array<box<Token>> &token_results) {
        for (auto &token : token_results) {
            auto tok = allocator->create_token();
            *tok = *token;
            tokens.add(tok);
        }
    }
};

class Parser {
    ParseContext *m_ctx;
    size_t m_toki = 0;
    Token *m_eof_token;
    map<Node *, size_t> m_block_pos;

    Token *next();

    Token *read();

    Token *get();

    Token *lookahead(int n);

    void jump_to(size_t pos);

    void unread();

    Token *expect(TokenType expected);

    Token *expect_identifier();

    void expected_got(TokenType expected, Token *token);

    void save_block_pos(Node *node) { m_block_pos[node] = m_toki; }

    void consume() { read(); }

    template <typename... Args> void error(Token *token, const char *format, const Args &...args) {
        auto message = fmt::format(format, args...);
        if (auto fn = m_ctx->error_handler) {
            Error error{message, *token};
            (*fn)(error);
            return;
        }
        print("{}:{}:{}: error: {}\n", m_ctx->module->full_path(), token->pos.line_number(),
              token->pos.col_number(), message);
        if (m_ctx->debug_mode) {
            panic("parser error");
        } else {
            exit(1);
        }
    }

  public:
    Parser(ParseContext *ctx);

    Node *create_node(NodeType type, Token *token);

    Node *create_struct_node(Token *keyword, const string &name);

    Node *create_identifier_node(Token *iden, Node *decl);

    Node *create_unary_expr_node(Token *token);

    ContainerKind get_container_kind(TokenType keyword);

    Node *create_error_node();

    void unexpected(Token *token);

    bool at_comma(TokenType end_token);

    int get_op_precedence(TokenType op_type);

    bool next_is(TokenType token_type);

    void add_to_scope(Node *node);

    void add_to_scope(Node *node, const string &name);

    Scope *get_scope() { return m_ctx->resolver->get_scope(); }

    void parse();

    Node *parse_root();

    void parse_top_level_decls(NodeList *decls);

    DeclSpec *parse_decl_spec(DeclSpec *spec = {});

    void parse_attributes(NodeList *attributes);
    Node *parse_attribute();

    Node *parse_top_level_decl(DeclSpec *decl_spec = nullptr);

    FnKind parse_fn_identifier(Token **iden);

    Node *parse_identifier();

    IdentifierKind get_identifier_kind(Node *node);

    Node *parse_type_expr();

    Node *parse_fn_lambda();

    Node *parse_fn_decl(uint32_t flags, DeclSpec *decl_spec = nullptr);

    void parse_fn_block(Node *fn);

    Node *parse_var_identifier();

    optional<SigilKind> get_sigil_kind(TokenType token_type);

    Node *parse_var_decl(bool as_field, DeclSpec *decl_spec = nullptr);

    Node *parse_fn_proto(Token *iden);

    Node *parse_fn_type(Token *func);

    bool parse_fn_params(NodeList *params);

    Node *parse_fn_param();

    Node *parse_block(Scope *scope = nullptr, Token *arrow = nullptr);

    Node *parse_stmt(bool *as_expr);

    Node *parse_expr();

    Node *parse_expr_clause(bool lhs);

    Node *parse_binary_expr(bool lhs, Node *parent, int prec);

    Node *parse_child_expr(bool lhs, Node *parent);

    Node *parse_unary_expr(bool lhs, Node *parent);

    Node *parse_primary_expr(bool lhs, Node *parent);

    Node *parse_operand(bool lhs, Node *parent);

    Node *parse_fn_call_expr(Node *fn_expr, bool lhs, Node *parent);

    Node *parse_simple_stmt();

    Node *parse_return_stmt();

    Node *parse_branch_stmt();

    Node *parse_if_stmt();

    Node *parse_for_stmt();

    void skip_block();

    Node *parse_struct_member(ContainerKind container_kind);

    void parse_struct_block(Node *node);

    Node *parse_struct_decl(TokenType keyword, DeclSpec *decl_spec = nullptr);

    Node *parse_construct_expr();

    Node *parse_prefix_expr();

    Node *parse_sizeof_expr();

    Node *parse_dot_expr(Node *expr);

    Node *parse_index_expr(Node *expr);

    Node *parse_typedef();

    Node *parse_enum_member();

    Node *parse_extern_decl();

    Node *parse_import_decl();

    Node *parse_switch_expr();

    Node *parse_case_expr();
};
} // namespace cx
