/*
 * Copyright (c) 2018 Hai Thanh Nguyen
 *
 * This file is part of chi, which is MIT licensed.
 * See http://opensource.org/licenses/MIT
 */

#include "lexer.h"
#include "fmt/core.h"

using namespace cx;

KeywordMap Lexer::s_keywords;

static bool is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

static bool is_letter(char c) {
    return ('a' <= c && c <= 'z') || ('A' <= c && c <= 'Z') || c == '_';
}

static bool is_digit(char c) { return '0' <= c && c <= '9'; }

Lexer::Lexer(io::Buffer *src, Tokenization *result) {
    m_src = src;
    m_result = result;
    reset();
    setup_keywords();
}

void Lexer::setup_keywords() {
    if (!s_keywords.is_empty()) {
        return;
    }
    s_keywords["var"] = TokenType::KW_VAR;
    s_keywords["break"] = TokenType::KW_BREAK;
    s_keywords["case"] = TokenType::KW_CASE;
    s_keywords["let"] = TokenType::KW_LET;
    s_keywords["continue"] = TokenType::KW_CONTINUE;
    s_keywords["else"] = TokenType::KW_ELSE;
    s_keywords["enum"] = TokenType::KW_ENUM;
    s_keywords["for"] = TokenType::KW_FOR;
    s_keywords["func"] = TokenType::KW_FUNC;
    s_keywords["goto"] = TokenType::KW_GOTO;
    s_keywords["while"] = TokenType::KW_WHILE;
    s_keywords["if"] = TokenType::KW_IF;
    s_keywords["private"] = TokenType::KW_PRIVATE;
    s_keywords["protected"] = TokenType::KW_PROTECTED;
    s_keywords["return"] = TokenType::KW_RETURN;
    s_keywords["static"] = TokenType::KW_STATIC;
    s_keywords["select"] = TokenType::KW_SELECT;
    s_keywords["struct"] = TokenType::KW_STRUCT;
    s_keywords["interface"] = TokenType::KW_INTERFACE;
    s_keywords["switch"] = TokenType::KW_SWITCH;
    s_keywords["typedef"] = TokenType::KW_TYPEDEF;
    s_keywords["typeof"] = TokenType::KW_TYPEOF;
    s_keywords["new"] = TokenType::KW_NEW;
    s_keywords["delete"] = TokenType::KW_DELETE;
    s_keywords["this"] = TokenType::KW_THIS;
    s_keywords["template"] = TokenType::KW_TEMPLATE;
    s_keywords["union"] = TokenType::KW_UNION;
    s_keywords["try"] = TokenType::KW_TRY;
    s_keywords["import"] = TokenType::KW_IMPORT;
    s_keywords["as"] = TokenType::KW_AS;
    s_keywords["sizeof"] = TokenType::KW_SIZEOF;
    s_keywords["extern"] = TokenType::KW_EXTERN;
    s_keywords["export"] = TokenType::KW_EXPORT;
    s_keywords["implements"] = TokenType::KW_IMPLEMENTS;
    s_keywords["mut"] = TokenType::KW_MUT;
    s_keywords["in"] = TokenType::KW_IN;

    // bool
    s_keywords["true"] = TokenType::BOOL;
    s_keywords["false"] = TokenType::BOOL;
    s_keywords["null"] = TokenType::NULLP;
}

string &Lexer::new_buf(size_t reserve) {
    m_cbuf.clear();
    m_cbuf.reserve(reserve);
    return m_cbuf;
}

void Lexer::next(Token *tok) {
    m_tok.type = TokenType::END;
    m_tok.val.d = 0.;
    m_tok.str.clear();

    next();

    *tok = std::move(m_tok);
}

void Lexer::next() {
l0:
    // skip white space
    char c;
    do {
        c = read();
    } while (is_space(c));

    m_tok.pos = pos();

    // identifiers and keywords
    if (is_letter(c)) {
        read_iden(c);
        return;
    }

    char c1 = 0;

    if (m_eof) {
        return;
    } else if (is_digit(c)) {
        read_number(c);
    } else if (c == '.') {
        c1 = read();
        if (is_digit(c1)) {
            unread();
            read_number('.');
            return;
        }

        if (c1 == '.') {
            auto p = peek();
            if (p == '.') {
                read();
                m_tok.type = TokenType::ELLIPSIS;
                return;
            }

            unread();
        }

        unread();
        m_tok.type = TokenType::DOT;
    } else if (c == '"') {
        read_string();
    } else if (c == '`') {
        read_raw_string();
    } else if (c == '\'') {
        read_rune();
    } else if (c == '#') {
        c = read();
        while (c != '\n') {
            c = read(); // ignore
        }
        goto l0;
    } else if (c == '/') {
        if (read_expect('/')) {
            c = read();
            while (c && c != '\n') {
                c = read(); // ignore
            }
            goto l0;
        } else if (read_expect('*')) {
            c = read();
            while (1) {
                if (c == '*') {
                    c = read();
                    if (c == '/') {
                        break;
                    }
                    continue;
                }
                if (m_eof) {
                    error("eof in comment", pos());
                    return;
                }

                c = read(); // ignore
            }
            goto l0;
        }

        m_tok.type = read_rep('=', TokenType::DIV_ASS, TokenType::DIV);
    } else if (c == ':') {
        m_tok.type = TokenType::COLON;
    } else if (c == '*') {
        m_tok.type = read_rep('=', TokenType::MUL_ASS, TokenType::MUL);
    } else if (c == '%') {
        m_tok.type = read_rep('=', TokenType::MOD_ASS, TokenType::MOD);
    } else if (c == '+') {
        if (read_expect('+')) {
            m_tok.type = TokenType::INC;
        } else {
            m_tok.type = read_rep('=', TokenType::ADD_ASS, TokenType::ADD);
        }
    } else if (c == '-') {
        if (read_expect('-')) {
            m_tok.type = TokenType::DEC;
        } else {
            m_tok.type = read_rep('=', TokenType::SUB_ASS, TokenType::SUB);
        }
    } else if (c == '>') {
        if (read_expect('>')) {
            if (read_expect('=')) {
                m_tok.type = TokenType::RSHIFT_ASS;
            } else {
                unread();
                m_tok.type = TokenType::GT;
            }
        } else {
            m_tok.type = read_rep('=', TokenType::GE, TokenType::GT);
        }
    } else if (c == '<') {
        if (read_expect('<')) {
            m_tok.type = read_rep('=', TokenType::LSHIFT_ASS, TokenType::LSHIFT);
        } else {
            m_tok.type = read_rep('=', TokenType::LE, TokenType::LT);
        }
    } else if (c == '=') {
        if (read_expect('>')) {
            m_tok.type = TokenType::ARROW;
        } else {
            m_tok.type = read_rep('=', TokenType::EQ, TokenType::ASS);
        }
    } else if (c == '!') {
        m_tok.type = read_rep('=', TokenType::NE, TokenType::LNOT);
    } else if (c == '&') {
        m_tok.type = read_rep('&', TokenType::LAND, TokenType::AND);
        m_tok.type = read_rep('=', TokenType::AND_ASS, m_tok.type);
    } else if (c == '|') {
        m_tok.type = read_rep('|', TokenType::LOR, TokenType::OR);
        m_tok.type = read_rep('=', TokenType::OR_ASS, m_tok.type);
    } else if (c == '^') {
        m_tok.type = read_rep('=', TokenType::XOR_ASS, TokenType::XOR);
    } else if (c == '~') {
        m_tok.type = TokenType::NOT;
    } else if (c == '@') {
        m_tok.type = TokenType::AT;

    } else {
        auto &t = m_tok.type;
        switch (c) {
        case '(':
            t = TokenType::LPAREN;
            break;
        case ')':
            t = TokenType::RPAREN;
            break;
        case '[':
            t = TokenType::LBRACK;
            break;
        case ']':
            t = TokenType::RBRACK;
            break;
        case '{':
            t = TokenType::LBRACE;
            break;
        case '}':
            t = TokenType::RBRACE;
            break;
        case ',':
            t = TokenType::COMMA;
            break;
        case ';':
            t = TokenType::SEMICOLON;
            break;
        case '?':
            t = TokenType::QUES;
            break;
        default:
            error(fmt::format("unexpected character '{}'", c), pos());
        }
    }
}

bool Lexer::read_expect(char expect) {
    auto c = read();
    if (c == expect) {
        return true;
    }
    unread();
    return false;
}

TokenType Lexer::read_rep(char expect, TokenType t_if, TokenType t_else) {
    auto c = read();
    if (c == expect) {
        return t_if;
    }
    unread();
    return t_else;
}

void Lexer::read_rune() {
    char c;
    bool ok = read_char('\'', &c);
    if (!ok) {
        error("empty character literal or unescaped ' in character literal", pos());
        c = '\'';
    }

    auto c1 = read();
    if (c1 != '\'') {
        error("missing '", pos());
        unread();
    }

    m_tok.type = TokenType::CHAR;
    m_tok.val.i = c;
}

void Lexer::read_raw_string() {
    auto buf = new_buf();
    while (1) {
        auto c = read();
        if (c == '\r') {
            continue;
        }
        if (m_eof) {
            error("eof in string", pos());
            break;
        }
        if (c == '`') {
            break;
        }
        buf.push_back(c);
    }

    m_tok.str = buf;
    m_tok.type = TokenType::STRING;
}

void Lexer::read_string() {
    auto buf = new_buf();
    while (1) {
        char c;
        bool ok = read_char('"', &c);
        if (!ok) {
            break;
        }

        buf.push_back(c);
    }

    m_tok.str = buf;
    m_tok.type = TokenType::STRING;
    m_tok.val.i = 0;
}

bool Lexer::read_char(char quote, char *out) {
    auto c = read();
    bool ok = true;

    if (m_eof) {
        error("eof in string", pos());
        unread();
        return false;
    }

    switch (c) {
    case '\n':
        error("newline in string", pos());
        unread();
        return false;

    case '\\':
        break;

    default:
        if (c == quote) {
            return false;
        }

        *out = c;
        return true;
    }

    c = read();
    switch (c) {
    case 'x':
        *out = read_hex_char(2);
        return true;

    case 'u':
        *out = read_unicode_char(4);
        return true;

    case 'U':
        *out = read_unicode_char(8);
        return true;

    case 'a':
        c = '\a';
        break;
    case 'b':
        c = '\b';
        break;
    case 'f':
        c = '\f';
        break;
    case 'n':
        c = '\n';
        break;
    case 'r':
        c = '\r';
        break;
    case 't':
        c = '\t';
        break;
    case 'v':
        c = '\v';
        break;
    case '\\':
        c = '\\';
        break;

    default:
        if (c >= '0' && c <= '7') {
            auto x = c - '0';
            auto p = pos();
            for (long i = 2; i > 0; i--) {
                c = read();
                if (c >= '0' && c <= '7') {
                    x = x * 8 + c - '0';
                    continue;
                }

                error(fmt::format("non-octal character in escape sequence: '{}'", c), pos());
                unread();
                ok = false;
            }

            if (x > 255) {
                error(fmt::format("octal escape value > 255: {}", x), p);
                ok = false;
            }

            *out = x;
            return ok;
        }

        if (c != quote) {
            error(fmt::format("unknown escape sequence: {}", c), pos());
            ok = false;
        }
    }

    *out = c;
    return ok;
}

uint32_t Lexer::read_unicode_char(long n) {
    auto p = pos();
    auto x = read_hex_char(n);
    if (x > UTF8_MAX || (0xd800 <= x && x < 0xe000)) {
        error(fmt::format("invalid Unicode code point in escape sequence: {:#x}", x), p);
        return 0;
    }
    return x;
}

uint32_t Lexer::read_hex_char(long n) {
    uint32_t x = 0;

    for (; n > 0; n--) {
        uint32_t d;
        auto c = read();
        if (is_digit(c)) {
            d = c - '0';
        } else if ('a' <= c && c <= 'f') {
            d = c - 'a' + 10;
        } else if ('A' <= c && c <= 'F') {
            d = c - 'A' + 10;
        } else {
            error(fmt::format("non-hex character in escape sequence: '{}'", c), pos());
            unread();
            return x;
        }
        x = x * 16 + d;
    }

    return x;
}

void Lexer::read_number(char c) {
    auto buf = new_buf();
    // parse mantissa before decimal point or exponent
    bool is_int = false;
    char malformed_octal = 0;
    long b = 10;
    auto p = pos();

    if (c != '.') {
        if (c != '0') {
            // decimal or float
            while (is_digit(c)) {
                buf.push_back(c);
                c = read();
            };
        } else {
            // c == 0
            auto p = pos();
            buf.push_back(c);
            c = read();
            if (c == 'b' || c == 'B') {
                is_int = true;
                b = 2;
                while (is_digit(c)) {
                    if (c > '1') {
                        error(fmt::format("invalid digit '{}' in binary constant", c), pos());
                        return;
                    }
                    buf.push_back(c);
                    c = read();
                }
            } else if (c == 'x' || c == 'X') {
                b = 16;
                is_int = true; // must be long
                c = read();
                while (is_digit(c) || ('a' <= c && c <= 'f') || ('A' <= c && c <= 'F')) {
                    buf.push_back(c);
                    c = read();
                };
                if (buf.size() < 2) {
                    error("malformed hex constant", p);
                    return;
                }
            } else if (is_digit(c)) {
                // octal
                b = 8;
                while (is_digit(c)) {
                    if (c > '7') {
                        if (!malformed_octal) {
                            malformed_octal = c;
                        }
                    }
                    buf.push_back(c);
                    c = read();
                }
            } else {
                unread();
                do {
                    c = read();
                } while (is_letter(c));
            }
        }
    }

    // unless we have a hex or binary number, parse fractional part or exponent,
    // if any
    if (!is_int) {
        is_int = true; // assume long unless proven otherwise

        // fraction
        if (c == '.') {
            is_int = false;
            buf.push_back(c);
            c = read();
            while (is_digit(c)) {
                buf.push_back(c);
                c = read();
            }
        }

        // exponent
        if (c == 'e' || c == 'E') {
            buf.push_back(c);
            is_int = false;
            c = read();
            if (c == '+' || c == '-') {
                buf.push_back(c);
                c = read();
            }
            if (!is_digit(c)) {
                error("malformed floating point constant exponent", pos());
                return;
            }
            while (is_digit(c)) {
                buf.push_back(c);
                c = read();
            }
        }

        if (is_letter(c)) {
            error(fmt::format("invalid numeric literal operator '{}'", c), pos());
            return;
        }
    }

    unread();

    if (is_int) {
        if (malformed_octal) {
            error(fmt::format("invalid digit '{}' in octal constant", malformed_octal), pos());
            return;
        }

        auto i = std::strtoull(buf.c_str(), NULL, b);
        if (i == ULLONG_MAX) {
            error(fmt::format("integer constant {} is too large", buf), p);
        }
        m_tok.val.i = i;
        m_tok.type = TokenType::INT;
        return;
    } else { // float
        double d = std::strtod(buf.c_str(), NULL);
        if (d == HUGE_VAL) {
            error(fmt::format("floating point constant {} is too large", buf), p);
        }

        m_tok.val.d = d;
        m_tok.type = TokenType::FLOAT;
        return;
    }

    return;
}

void Lexer::read_iden(char c) {
    auto buf = new_buf();

    while (is_letter(c) || is_digit(c)) {
        buf.push_back(c);
        c = read();
    }

    unread();
    if (buf.length() >= 2) {
        auto kw = s_keywords.get(buf);
        if (kw) {
            m_tok.type = *kw;
            if (m_tok.type == TokenType::BOOL) {
                m_tok.val.b = buf == "true" ? true : false;
            }
            m_tok.str = buf;
            return;
        }
    }

    m_tok.str = buf;
    m_tok.type = TokenType::IDEN;
}

void Lexer::reset() {
    m_src->reset();
    m_eof = false;
    m_bufn = m_bufi = 0;
    for (long i = 0; i < BUF_LEN; i++) {
        m_pbuf[i].line = 0;
        m_pbuf[i].col = 0;
        m_pbuf[i].offset = -1;
    }
}

char Lexer::read() {
    // if there are characters put back
    if (m_bufn > 0) {
        m_bufi = (m_bufi + 1) % BUF_LEN;
        m_bufn--;
        return m_buf[m_bufi];
    }

    char ch = 0;
    // otherwise read from input
    auto err = m_src->read(&ch);
    if (err) {
        if (err == io::eof) {
            m_eof = true;
        }
        return 0;
    }

    auto p = m_pbuf[m_bufi];
    if (ch == '\n') {
        p.line++;
        p.col = 0;
    } else {
        p.col++;
    }
    p.offset++;

    m_bufi = (m_bufi + 1) % BUF_LEN;
    m_buf[m_bufi] = ch;
    m_pbuf[m_bufi] = p;

    return ch;
}

void Lexer::unread() {
    m_bufi = (m_bufi + BUF_LEN - 1) % BUF_LEN; // circularly decreases the index
    m_bufn++;
}

char Lexer::peek() {
    auto c = read();
    unread();
    return c;
}

Token Lexer::get() { return m_tok; }

void Lexer::tokenize() {
    for (;;) {
        auto tok = m_result->tokens.emplace(new Token(TokenType::END))->get();
        next(tok);
        if (m_eof) {
            break;
        }
    }
}

void Lexer::error(string error, Pos pos) {
    m_result->error = error;
    m_result->error_pos = pos;
}

string cx::get_token_symbol(TokenType token_type) {
    switch (token_type) {
    case TokenType::ADD:
        return "+";
    case TokenType::SUB:
        return "-";
    case TokenType::MUL:
        return "*";
    case TokenType::DIV:
        return "/";
    case TokenType::MOD:
        return "%";
    case TokenType::OR:
        return "|";
    case TokenType::AND:
        return "&";
    case TokenType::NOT:
        return "~";
    case TokenType::XOR:
        return "^";
    case TokenType::LSHIFT:
        return "<<";
    case TokenType::RSHIFT:
        return ">>";
    case TokenType::LOR:
        return "||";
    case TokenType::LAND:
        return "&&";
    case TokenType::LNOT:
        return "!";
    case TokenType::LT:
        return "<";
    case TokenType::LE:
        return "<=";
    case TokenType::GT:
        return ">";
    case TokenType::GE:
        return ">=";
    case TokenType::EQ:
        return "==";
    case TokenType::NE:
        return "!=";
    case TokenType::ASS:
        return "=";
    case TokenType::MUL_ASS:
        return "*=";
    case TokenType::DIV_ASS:
        return "/=";
    case TokenType::MOD_ASS:
        return "%=";
    case TokenType::ADD_ASS:
        return "+=";
    case TokenType::SUB_ASS:
        return "-=";
    case TokenType::LSHIFT_ASS:
        return "<<=";
    case TokenType::RSHIFT_ASS:
        return ">>=";
    case TokenType::AND_ASS:
        return "&=";
    case TokenType::XOR_ASS:
        return "^=";
    case TokenType::OR_ASS:
        return "|=";
    case TokenType::INC:
        return "++";
    case TokenType::DEC:
        return "--";
    case TokenType::LPAREN:
        return "(";
    case TokenType::RPAREN:
        return ")";
    case TokenType::LBRACK:
        return "[";
    case TokenType::RBRACK:
        return "]";
    case TokenType::LBRACE:
        return "{";
    case TokenType::RBRACE:
        return "}";
    case TokenType::COMMA:
        return ",";
    case TokenType::DOT:
        return ".";
    case TokenType::COLON:
        return ":";
    case TokenType::SEMICOLON:
        return ";";
    case TokenType::ELLIPSIS:
        return "...";
    case TokenType::TILDE:
        return "~";
    case TokenType::QUES:
        return "?";
    case TokenType::AT:
        return "@";
    default:
        return PRINT_ENUM(token_type);
    }
}

string cx::get_strlit_repr(const string &str) {
    stringstream out;
    for (auto c : str) {
        switch (c) {
        case '"':
            out << "\\\"";
            break;
        case '\t':
            out << "\\t";
            break;
        case '\n':
            out << "\\n";
            break;
        default:
            out.put(c);
        }
    }
    return out.str();
}

string Token::get_name() const {
    switch (type) {
    case TokenType::IDEN:
        return str;
    case TokenType::KW_NEW:
        return "new";
    case TokenType::KW_DELETE:
        return "delete";
    default:
        return "";
    }
}

string Token::to_string() const {
    switch (type) {
    case TokenType::IDEN:
        return str;
    case TokenType::CHAR:
        return fmt::format("'{}'", (char)val.i);
    case TokenType::STRING:
        return fmt::format("\"{}\"", get_strlit_repr(str));
    case TokenType::INT:
        return fmt::format("{}", val.i);
    case TokenType::FLOAT:
        return fmt::format("{}", val.d);
    case TokenType::BOOL:
        return val.b ? "true" : "false";
    case TokenType::NULLP:
        return "null";
    default:
        if (type >= TokenType::KW_BREAK && type <= TokenType::KW_UNION) {
            return str;
        }
        return get_token_symbol(type);
    }
}

TokenType cx::get_assignment_op(TokenType token_type) {
    switch (token_type) {
    case TokenType::ASS:
        return token_type;
    case TokenType::ADD_ASS:
        return TokenType::ADD;
    case TokenType::SUB_ASS:
        return TokenType::SUB;
    case TokenType::MUL_ASS:
        return TokenType::MUL;
    case TokenType::DIV_ASS:
        return TokenType::DIV;
    case TokenType::MOD_ASS:
        return TokenType::MOD;
    case TokenType::LSHIFT_ASS:
        return TokenType::LSHIFT;
    case TokenType::RSHIFT_ASS:
        return TokenType::RSHIFT;
    case TokenType::AND_ASS:
        return TokenType::AND;
    case TokenType::OR_ASS:
        return TokenType::OR;
    case TokenType::XOR_ASS:
        return TokenType::XOR;
    default:
        return TokenType::END;
    }
}

bool cx::is_assignment_op(TokenType tokenType) {
    return get_assignment_op(tokenType) != TokenType::END;
}