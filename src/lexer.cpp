// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 Lucas Holt. All rights reserved.

#include "lexer.h"

#include <cctype>
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
        if (c == ' ' || c == '\t') {
            size_t j = i;
            while (j < n && (src[j] == ' ' || src[j] == '\t')) j++;
            if (j < n && src[j] == '_' && j + 1 < n && (src[j + 1] == '\n' || (src[j + 1] == '\r' && j + 2 < n && src[j + 2] == '\n'))) {
                // skip whitespace, underscore, and the newline
                i = j + 1;
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
    auto push = [&](TK k, const std::string& t, double num = 0, bool isInt = false) {
        Token tk; tk.k = k; tk.text = t; tk.num = num; tk.isInt = isInt; tk.line = line;
        tk.spaceBefore = sawSpace;
        toks.push_back(std::move(tk));
        sawSpace = false;
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

        // hex / octal literal: &H.. &O..
        if (c == '&' && i + 2 < n && (src[i + 1] == 'H' || src[i + 1] == 'h') &&
            std::isxdigit((unsigned char)src[i + 2])) {
            size_t j = i + 2;
            while (j < n && std::isxdigit((unsigned char)src[j])) j++;
            std::string hex = src.substr(i + 2, j - i - 2);
            push(TK::Number, "&H" + hex, (double)std::strtoll(hex.c_str(), nullptr, 16), true);
            i = j;
            continue;
        }
        if (c == '&' && i + 2 < n && (src[i + 1] == 'O' || src[i + 1] == 'o') &&
            src[i + 2] >= '0' && src[i + 2] <= '7') {
            size_t j = i + 2;
            while (j < n && src[j] >= '0' && src[j] <= '7') j++;
            std::string oct = src.substr(i + 2, j - i - 2);
            push(TK::Number, "&O" + oct, (double)std::strtoll(oct.c_str(), nullptr, 8), true);
            i = j;
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
            if (lw == "rem" && (i >= n || src[i] == ' ' || src[i] == '\t' || src[i] == '\n')) {
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
        if (c == '<') {
            if (i + 1 < n && src[i + 1] == '>') { push(TK::Op, "<>"); i += 2; continue; }
            if (i + 1 < n && src[i + 1] == '=') { push(TK::Op, "<="); i += 2; continue; }
            push(TK::Op, "<"); i++; continue;
        }
        if (c == '>') {
            if (i + 1 < n && src[i + 1] == '=') { push(TK::Op, ">="); i += 2; continue; }
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
