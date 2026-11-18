// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 Lucas Holt. All rights reserved.

#include "lexer.h"

#include "variant.h"

#include <cctype>
#include <cstdint>
#include <cstdlib>

static std::string joinContinuations(const std::string& src) {
    std::string out;
    out.reserve(src.size());
    size_t i = 0, n = src.size();
    while (i < n) {
        char c = src[i];
        if (c == '\n') { out += '\n'; i++; continue; }
        if (c == '\r') { i++; continue; }
        // detect " _" at end of line (underscore preceded by whitespace, then EOL)
        // line continuation: a '_' followed only by whitespace and a newline,
        // regardless of what precedes it (VBScript allows "x =_")
        if (c == '_') {
            size_t j = i + 1;
            while (j < n && (src[j] == ' ' || src[j] == '\t')) j++;
            if (j >= n || src[j] == '\n' || src[j] == '\r') {
                i = j;
                while (i < n && (src[i] == '\r' || src[i] == '\n')) i++;
                out += ' ';
                continue;
            }
        }
        out += c;
        i++;
    }
    return out;
}

std::vector<Token> lex(const std::string& srcIn) {
    std::string src = joinContinuations(srcIn);
    std::vector<Token> toks;
    size_t i = 0, n = src.size();
    int line = 1;

    bool sawSpace = false;
    bool afterDot = false; // previous token was '.', so keywords are member names
    auto push = [&](TK k, const std::string& t, double num = 0, bool isInt = false, bool isDate = false) {
        Token tk; tk.k = k; tk.text = t; tk.num = num; tk.isInt = isInt; tk.isDate = isDate; tk.line = line;
        tk.spaceBefore = sawSpace;
        toks.push_back(std::move(tk));
        sawSpace = false;
        afterDot = (k == TK::Op && t == ".");
    };

    while (i < n) {
        char c = src[i];
        if (c == '\n') { push(TK::Newline, "\n"); line++; i++; continue; }
        if (c == ' ' || c == '\t' || c == '\r') { sawSpace = true; i++; continue; }

        // comment: ' ...
        if (c == '\'') {
            while (i < n && src[i] != '\n') i++;
            continue;
        }

        // string literal
        if (c == '"') {
            i++;
            std::string val;
            while (i < n) {
                if (src[i] == '"') {
                    if (i + 1 < n && src[i + 1] == '"') { val += '"'; i += 2; continue; }
                    i++;
                    break;
                }
                if (src[i] == '\n') throw ParseError{line, "unterminated string literal"};
                val += src[i];
                i++;
            }
            push(TK::String, val);
            continue;
        }

        // hex / octal literal: &H.. / &H..& / &O.. / &O..&
        // With the trailing '&' the value is a Long (wraps at 32 bits, so
        // &HFFFFFFFF& = -1); without it it is an Integer (wraps at 16 bits,
        // so &HFFFF = -1 but &HFFFF& = 65535). Larger values are Double.
        auto pushBased = [&](const std::string& digits, int base, const std::string& tag) {
            size_t j = i + 2;
            auto okDigit = [&](char ch) {
                return base == 16 ? std::isxdigit((unsigned char)ch)
                                  : (ch >= '0' && ch <= '7');
            };
            while (j < n && okDigit(src[j])) j++;
            std::string d = src.substr(i + 2, j - i - 2);
            bool suffix = (j < n && src[j] == '&');
            if (suffix) j++;
            unsigned long long v = std::strtoull(d.c_str(), nullptr, base);
            double num;
            bool isInt;
            if (!suffix && v <= 0xFFFFULL) { num = (double)(int16_t)(uint16_t)v; isInt = true; }
            else if (v <= 0xFFFFFFFFULL) { num = (double)(int32_t)(uint32_t)v; isInt = true; }
            else { num = (double)v; isInt = false; }
            push(TK::Number, tag + d + (suffix ? "&" : ""), num, isInt);
            i = j;
        };
        if (c == '&' && i + 2 < n && (src[i + 1] == 'H' || src[i + 1] == 'h') &&
            std::isxdigit((unsigned char)src[i + 2])) { pushBased("", 16, "&H"); continue; }
        if (c == '&' && i + 2 < n && (src[i + 1] == 'O' || src[i + 1] == 'o') &&
            src[i + 2] >= '0' && src[i + 2] <= '7') { pushBased("", 8, "&O"); continue; }

        // date literal: #...#  (contents parsed as a date/time)
        if (c == '#') {
            size_t j = i + 1;
            while (j < n && src[j] != '#' && src[j] != '\n') j++;
            if (j >= n || src[j] != '#') throw ParseError{line, "unterminated date literal"};
            std::string inner = src.substr(i + 1, j - i - 1);
            double serial = 0;
            if (!parseDateString(inner, serial))
                throw ParseError{line, "invalid date literal #" + inner + "#"};
            push(TK::Number, "#" + inner + "#", serial, false, true);
            i = j + 1;
            continue;
        }

        // number: digits [. digits] [E[+-]digits]  or  .digits
        if (std::isdigit((unsigned char)c) ||
            (c == '.' && i + 1 < n && std::isdigit((unsigned char)src[i + 1]))) {
            size_t j = i;
            bool isInt = true;
            while (j < n && std::isdigit((unsigned char)src[j])) j++;
            if (j < n && src[j] == '.') {
                isInt = false;
                j++;
                while (j < n && std::isdigit((unsigned char)src[j])) j++;
            }
            if (j < n && (src[j] == 'e' || src[j] == 'E')) {
                size_t k = j + 1;
                if (k < n && (src[k] == '+' || src[k] == '-')) k++;
                if (k < n && std::isdigit((unsigned char)src[k])) {
                    isInt = false;
                    j = k;
                    while (j < n && std::isdigit((unsigned char)src[j])) j++;
                }
            }
            std::string text = src.substr(i, j - i);
            push(TK::Number, text, std::strtod(text.c_str(), nullptr), isInt);
            i = j;
            continue;
        }

        // identifier / keyword / Rem / bracketed identifier
        if (std::isalpha((unsigned char)c) || c == '_') {
            size_t j = i;
            while (j < n && (std::isalnum((unsigned char)src[j]) || src[j] == '_')) j++;
            std::string word = src.substr(i, j - i);
            i = j;
            std::string lw = word;
            for (auto& ch : lw) ch = (char)std::tolower((unsigned char)ch);
            if (lw == "rem" && !afterDot && (i >= n || src[i] == ' ' || src[i] == '\t' || src[i] == '\n')) {
                while (i < n && src[i] != '\n') i++;
                continue;
            }
            push(TK::Ident, word);
            continue;
        }
        if (c == '[') {
            size_t j = i + 1;
            std::string val;
            while (j < n && src[j] != ']') {
                if (src[j] == '\n') throw ParseError{line, "unterminated bracketed identifier"};
                val += src[j];
                j++;
            }
            if (j >= n) throw ParseError{line, "unterminated bracketed identifier"};
            i = j + 1;
            push(TK::Ident, val);
            continue;
        }

        // operators
        if (c == ':') {
            if (i + 1 < n && src[i + 1] == '=') { push(TK::Op, ":="); i += 2; continue; }
            push(TK::Colon, ":"); i++; continue;
        }
        // VBScript lexer quirks: "=>" is ">=", "=<" is "<=", "><" is "<>"
        if (c == '=' && i + 1 < n && src[i + 1] == '>') { push(TK::Op, ">="); i += 2; continue; }
        if (c == '=' && i + 1 < n && src[i + 1] == '<') { push(TK::Op, "<="); i += 2; continue; }
        if (c == '<') {
            if (i + 1 < n && src[i + 1] == '>') { push(TK::Op, "<>"); i += 2; continue; }
            if (i + 1 < n && src[i + 1] == '=') { push(TK::Op, "<="); i += 2; continue; }
            push(TK::Op, "<"); i++; continue;
        }
        if (c == '>') {
            if (i + 1 < n && src[i + 1] == '=') { push(TK::Op, ">="); i += 2; continue; }
            if (i + 1 < n && src[i + 1] == '<') { push(TK::Op, "<>"); i += 2; continue; }
            push(TK::Op, ">"); i++; continue;
        }
        if (c == '+' || c == '-' || c == '*' || c == '/' || c == '\\' || c == '^' ||
            c == '&' || c == '=' || c == '(' || c == ')' || c == ',' || c == '.' ||
            c == '!' || c == ';') {
            push(TK::Op, std::string(1, c));
            i++;
            continue;
        }

        throw ParseError{line, std::string("invalid character '") + c + "'"};
    }

    Token end; end.k = TK::End; end.line = line;
    toks.push_back(end);
    return toks;
}
