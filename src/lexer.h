/*
 * Copyright (c) 2018 Hai Thanh Nguyen
 *
 * This file is part of chi, which is MIT licensed.
 * See http://opensource.org/licenses/MIT
 */

#pragma once

#include <functional>
#include <istream>
#include <string>
#include <unordered_map>

#include "compat.h"

namespace cx {

    MAKE_ENUM(TokenType, END, IDEN, KEYWORD,

    // literals
              INT,    // 322, 0322, 0xBadFace
              FLOAT,  // 322.0
              CHAR,   // '海'
              STRING, // "Hải"

    // operators
              ADD,    // +
              SUB,    // -
              MUL,    // *
              DIV,    // /
              MOD,    // %
              OR,     // |
              AND,    // &
              NOT,    // ~
              XOR,    // ^
              LSHIFT, // <<
              RSHIFT, // >>
              LOR,    // ||
              LAND,   // &&
              LNOT,   // !
              LT,     // <
              LE,     // <=
              GT,     // >
              GE,     // >=
              EQ,     // ==
              NE,     // !=

    // assignment
              ASS,        // =
              MUL_ASS,    // *=
              DIV_ASS,    // /=
              MOD_ASS,    // %=
              ADD_ASS,    // +=
              SUB_ASS,    // -=
              LSHIFT_ASS, // <<=
              RSHIFT_ASS, // >>=
              AND_ASS,    // &=
              XOR_ASS,    // ^=
              OR_ASS,     // |=

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
              TILDE      // ~
    )

    MAKE_ENUM(Keyword, LET, BREAK, CASE, CONST, CONTINUE, DEFAULT, ELSE, ENUM, FOR,
              FUNC, GOTO, WHILE, IF, INTERFACE, IMPL, PUBLIC, PRIVATE, FOREACH,
              RETURN, SELECT, STATIC, STRUCT, SWITCH, TYPEDEF, TYPEOF, NEW
    )

    struct Pos {
        int line, col, offset, file;

        Pos() {
            line = -1;
            col = -1;
            offset = -1;
            file = -1;
        }

        bool is_valid() { return offset >= 0; }
    };

    struct Token {
        union Value {
            double d;   // floating point value
            int64_t i;  // integer value
            Keyword kw; // keyword type
        } val;
        std::string str;
        TokenType type;
        Pos pos;

        std::string repr() const;

        Token(TokenType type = TokenType::END) { this->type = type; }
    };

    typedef std::function<void(std::string, Pos)> ErrorFunc;
    typedef std::unordered_map<std::string, Keyword> KeywordMap;
// typedef std::function<bool(std::string)> LookupFunc;

    const int BUF_LEN = 4;
    const uint32_t UTF8_MAX = U'\U0010FFFF';

    class Lexer {
        static KeywordMap s_keywords;

        ErrorFunc m_on_err;
        std::istream& m_file;
        int m_file_id;

        char m_buf[BUF_LEN];
        Pos m_pbuf[BUF_LEN];
        int m_bufn, m_bufi;

        std::string m_cbuf;
        bool m_eof;

        Token m_tok;

        std::string& new_buf(size_t reserve = 5);

        char read();

        void unread();

        char peek();

        void next();

        void seek(long offset);

        void read_iden(char c);

        void read_number(char c);

        void read_string();

        void read_raw_string();

        void read_rune();

        bool read_expect(char expect);

        TokenType read_rep(char expect, TokenType t_if, TokenType t_else);


        bool read_char(char quote, char* c);

        uint32_t read_unicode_char(int n);

        uint32_t read_hex_char(int n);

        void setup_keywords();

        Pos pos() { return m_pbuf[m_bufi]; }

    public:
        Lexer(std::istream& file, int file_id, ErrorFunc on_err);

        void reset(long offset = 0);

        void error(std::string error, Pos pos) { m_on_err(error, pos); }

        void next(Token* tok);

        Token get();

        bool is_eof() const { return m_eof; }
    };

}