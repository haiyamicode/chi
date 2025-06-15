/*
 * Copyright (c) 2018 Hai Thanh Nguyen
 *
 * This file is part of chi, which is MIT licensed.
 * See http://opensource.org/licenses/MIT
 */

#pragma once

#include "util.h"

namespace cx {

namespace ast {
struct Node; // forward declare
}

MAKE_ENUM(TokenType, END, IDEN, ERROR,

          // keywords
          KW_BREAK, KW_CASE, KW_VAR, KW_LET, KW_CONTINUE, KW_ELSE, KW_ENUM, KW_FOR, KW_FUNC,
          KW_GOTO, KW_WHILE, KW_IF, KW_PRIVATE, KW_PROTECTED, KW_RETURN, KW_SELECT, KW_STATIC,
          KW_STRUCT, KW_INTERFACE, KW_SWITCH, KW_TYPEDEF, KW_TYPEOF, KW_NEW, KW_DELETE, KW_THIS,
          KW_EXTERN, KW_INLINE, KW_UNION, KW_TEMPLATE, KW_TRY, KW_IMPORT, KW_AS, KW_SIZEOF,
          KW_EXPORT, KW_IMPLEMENTS, KW_MUT,

          // literals
          BOOL,   // true / false
          INT,    // 322, 0322, 0xBadFace
          FLOAT,  // 322.0
          CHAR,   // '海'
          STRING, // "Hải"
          NULLP,  // null

          // operators
          ADD,    // +
          SUB,    // -
          MUL,    // *
          DIV,    // /
          MOD,    // %
          LSHIFT, // <<
          RSHIFT, // >>
          AND,    // &
          OR,     // |
          XOR,    // ^
          NOT,    // ~
          LOR,    // ||
          LAND,   // &&
          LNOT,   // !
          LT,     // <
          LE,     // <=
          GT,     // >
          GE,     // >=
          EQ,     // ==
          NE,     // !=
          MUTREF, // &mut

          // assignment
          ASS,        // =
          ADD_ASS,    // +=
          SUB_ASS,    // -=
          MUL_ASS,    // *=
          DIV_ASS,    // /=
          MOD_ASS,    // %=
          LSHIFT_ASS, // <<=
          RSHIFT_ASS, // >>=
          AND_ASS,    // &=
          OR_ASS,     // |=
          XOR_ASS,    // ^=

          // increment / decrement
          INC, // ++
          DEC, // --

          // delimiters
          LPAREN,    // (
          RPAREN,    // )
          LBRACK,    // [
          RBRACK,    // ]
          LBRACE,    // {
          RBRACE,    // }
          COMMA,     // ,
          DOT,       // .
          COLON,     // :
          SEMICOLON, // ;
          ELLIPSIS,  // ...
          QUES,      // ?
          TILDE,     // ~
          AT,        // @
          ARROW      // =>
)

struct Pos {
    // these values start at 0
    long line = -1;
    long col = -1;
    long offset = -1;

    static Pos from_offset(long offset) { return {-1, -1, offset}; }

    bool is_valid() const { return offset >= 0; }

    // get values starting at 1
    long line_number() const { return line + 1; }
    long col_number() const { return col + 1; }

    bool operator<(const Pos &p) const { return offset < p.offset; }
    bool operator<=(const Pos &p) const { return offset <= p.offset; }

    bool is_in_range(Pos start, Pos end) { return start <= *this && *this <= end; }

    Pos add_offset(long delta) const { return {0, 0, offset + delta}; }
    Pos add_col(long delta) const { return {0, col + delta, -1}; }
    Pos add_line(long delta) const { return {line + delta, 0, -1}; }
};

struct Token {
    union Value {
        bool b;    // bool value
        double d;  // floating point value
        int64_t i; // integer value
    } val;
    string str = "";
    TokenType type = TokenType::ERROR;
    Pos pos = {};
    ast::Node *node = nullptr; // identifier node associated with this token

    string to_string() const;
    string get_name() const;

    Token(TokenType type = TokenType::END) {
        this->type = type;
        memset(&val, 0, sizeof(val));
        switch (type) {
        case TokenType::BOOL:
            val.b = false;
            break;
        case TokenType::NULLP:
            val.i = 0;
            break;
        case TokenType::INT:
            val.i = 0;
            break;
        case TokenType::FLOAT:
            val.d = 0.0;
            break;
        case TokenType::CHAR:
            val.i = 0;
            break;
        default:
            break;
        }
    }

    bool is_identifier() const {
        switch (type) {
        case TokenType::IDEN:
        case TokenType::KW_THIS:
        case TokenType::KW_NEW:
        case TokenType::KW_DELETE:
            return true;
        default:
            break;
        }
        return false;
    }
};

typedef func<void(string, Pos)> ErrorFunc;
typedef map<string, TokenType> KeywordMap;

const long BUF_LEN = 4;
const uint32_t UTF8_MAX = U'\U0010FFFF';

struct Error {
    string message = "";
    Pos pos = {};
    int range = 0;

    Error(string message, Token token) : message(message), pos(token.pos) {
        range = token.to_string().size();
    }
    Error(string message, Pos pos) : message(message), pos(pos) { range = 1; }
};

using ErrorHandler = std::function<void(Error)>;

struct Tokenization {
    array<box<Token>> tokens;
    optional<string> error;
    Pos error_pos;
};

class Lexer {
    static KeywordMap s_keywords;
    io::Buffer *m_src = nullptr;
    Tokenization *m_result = nullptr;

    char m_buf[BUF_LEN];
    Pos m_pbuf[BUF_LEN];
    long m_bufn, m_bufi;

    string m_cbuf;
    bool m_eof;

    Token m_tok;

    string &new_buf(size_t reserve = 5);

    char read();

    void unread();

    char peek();

    void next();

    void read_iden(char c);

    void read_number(char c);

    void read_string();

    void read_raw_string();

    void read_rune();

    bool read_expect(char expect);

    TokenType read_rep(char expect, TokenType t_if, TokenType t_else);

    bool read_char(char quote, char *c);

    uint32_t read_unicode_char(long n);

    uint32_t read_hex_char(long n);

    void setup_keywords();

    Pos pos() { return m_pbuf[m_bufi]; }

  public:
    Lexer(io::Buffer *src, Tokenization *result);

    void tokenize();

    void reset();

    void error(string error, Pos pos);

    void next(Token *tok);

    Token get();

    bool is_eof() const { return m_eof; }
};

string get_token_symbol(TokenType token_type);

string get_strlit_repr(const string &str);

TokenType get_assignment_op(TokenType token_type);

bool is_assignment_op(TokenType token_type);
} // namespace cx