// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 Lucas Holt. All rights reserved.

// lexer.h — tokenizer for VBScript source.
#pragma once

#include <string>
#include <vector>

enum class TK { Ident, Number, String, Op, Newline, Colon, End };

struct Token {
    TK k = TK::End;
    std::string text;       // raw text (Ident: original case; Op: the operator)
    double num = 0;         // Number value
    bool isInt = false;     // Number is an integer literal
    bool spaceBefore = false; // whitespace preceded this token on the line
    int line = 0;
};

struct ParseError {
    int line;
    std::string msg;
};

// Joins " _" line continuations, then tokenizes.
std::vector<Token> lex(const std::string& src);
