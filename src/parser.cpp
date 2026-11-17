// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 Lucas Holt. All rights reserved.

#include "parser.h"

#include <algorithm>

// ---------- token helpers ----------

Token& Parser::peek(size_t off) {
    size_t idx = p + off;
    if (idx >= t.size()) idx = t.size() - 1;
    return t[idx];
}
const Token& Parser::peek(size_t off) const {
    size_t idx = p + off;
    if (idx >= t.size()) idx = t.size() - 1;
    return t[idx];
}
Token& Parser::advance() { return t[p < t.size() - 1 ? p++ : p]; }
bool Parser::at(TK k) const { return peek().k == k; }
bool Parser::atOp(const std::string& s) const { return peek().k == TK::Op && peek().text == s; }
bool Parser::atIdent(const std::string& kw) const {
    return peek().k == TK::Ident && lower(peek().text) == kw;
}
std::string Parser::identText() const {
    return peek().k == TK::Ident ? lower(peek().text) : "";
}
[[noreturn]] void Parser::fail(const std::string& msg) {
    throw ParseError{peek().line, msg + " (found '" +
                     (peek().k == TK::End ? std::string("<eof>") : peek().text) + "')"};
}
void Parser::expectOp(const std::string& s) {
    if (!atOp(s)) fail("expected '" + s + "'");
    advance();
}
void Parser::expectIdent(const std::string& kw) {
    if (!atIdent(kw)) fail("expected '" + kw + "'");
    advance();
}
void Parser::skipSeparators() {
    while (peek().k == TK::Newline || peek().k == TK::Colon) advance();
}

// ---------- top level ----------

Program Parser::parseProgram() {
    Program prog;
    prog.stmts = parseStatements({});
    return prog;
}

ExprPtr Parser::parseExpressionString(const std::string& src) {
    Parser p(lex(src));
    ExprPtr e = p.parseExpr();
    p.skipSeparators();
    if (p.peek().k != TK::End) p.fail("unexpected trailing input");
    return e;
}

Program Parser::parseStatementsString(const std::string& src) {
    Parser p(lex(src));
    Program prog;
    prog.stmts = p.parseStatements({});
    return prog;
}

// ---------- statements ----------

static bool isStmtKeyword(const std::string& w) {
    static const std::set<std::string> kw = {
        "option","dim","public","private","static","redim","const","if","select",
        "for","do","while","exit","sub","function","property","class","end","call",
        "set","let","with","on","execute","executeglobal","randomize","erase","goto","stop"
    };
    return kw.count(w) != 0;
}

std::vector<StmtPtr> Parser::parseStatements(const std::set<std::string>& terminators) {
    std::vector<StmtPtr> out;
    for (;;) {
        skipSeparators();
        if (peek().k == TK::End) break;
        if (peek().k == TK::Ident) {
            std::string w = identText();
            if (terminators.count(w)) {
                if (w == "end") {
                    // "End <kw>" terminates; bare "End" is a statement
                    if (peek(1).k == TK::Ident) break;
                } else break;
            }
        }
        out.push_back(parseStatement());
    }
    return out;
}

std::vector<StmtPtr> Parser::parseInlineStatements() {
    std::vector<StmtPtr> out;
    for (;;) {
        out.push_back(parseStatement());
        if (peek().k == TK::Colon) { advance(); continue; }
        break;
    }
    return out;
}

StmtPtr Parser::parseStatement() {
    Token& tk = peek();
    int line = tk.line;

    // line label:  ident ':'   (non-keyword)  or  number ':'
    if (tk.k == TK::Ident && peek(1).k == TK::Colon && !isStmtKeyword(lower(tk.text))) {
        auto s = std::make_unique<Stmt>();
        s->k = Stmt::Label; s->line = line; s->name = lower(tk.text);
        advance(); advance();
        return s;
    }
    if (tk.k == TK::Number &&
        (peek(1).k == TK::Colon || peek(1).k == TK::Newline || peek(1).k == TK::End)) {
        // numeric line label: "100:" or a line with just "100"
        auto s = std::make_unique<Stmt>();
        s->k = Stmt::Label; s->line = line; s->name = tk.text;
        advance();
        if (peek().k == TK::Colon) advance();
        return s;
    }

    if (tk.k == TK::Ident) {
        std::string w = lower(tk.text);
        if (w == "option") {
            advance();
            std::string what = identText();
            auto s = std::make_unique<Stmt>();
            s->k = Stmt::Option; s->line = line; s->name = what;
            if (what == "explicit") { s->b = true; advance(); }
            else if (what == "base") {
                advance();
                if (peek().k != TK::Number) fail("expected 0 or 1");
                s->num = (long long)advance().num;
            } else if (what == "compare") {
                advance();
                std::string m = identText();
                advance();
                if (m == "binary") s->b = false;
                else if (m == "text") s->b = true;
                else fail("expected Binary or Text");
            } else fail("expected Explicit, Base or Compare");
            return s;
        }
        if ((w == "public" || w == "private") && peek(1).k == TK::Ident) {
            std::string w2 = lower(peek(1).text);
            if (w2 == "sub" || w2 == "function" || w2 == "property") {
                advance(); // public/private
                if (w2 == "sub") return parseProcDecl(true);
                if (w2 == "function") return parseProcDecl(false);
                return parsePropDecl();
            }
        }
        if (w == "dim" || w == "public" || w == "private" || w == "static") {
            advance();
            auto s = std::make_unique<Stmt>();
            s->k = Stmt::Dim; s->line = line; s->name = w; // "static" persists
            s->decls = parseDeclList(false);
            return s;
        }
        if (w == "redim") {
            advance();
            auto s = std::make_unique<Stmt>();
            s->k = Stmt::ReDim; s->line = line;
            if (atIdent("preserve")) { s->b = true; advance(); }
            s->decls = parseDeclList(true);
            return s;
        }
        if (w == "const") {
            advance();
            auto s = std::make_unique<Stmt>();
            s->k = Stmt::ConstDecl; s->line = line;
            for (;;) {
                if (peek().k != TK::Ident) fail("expected identifier");
                Decl d; d.name = lower(advance().text);
                expectOp("=");
                d.dynamic = false;
                // store the value expression in dims slot? use dedicated: reuse decls + exprs parallel
                s->decls.push_back(std::move(d));
                s->exprs.push_back(parseExpr());
                if (atOp(",")) { advance(); continue; }
                break;
            }
            return s;
        }
        if (w == "if") return parseIf();
        if (w == "select") return parseSelect();
        if (w == "for") return parseFor();
        if (w == "do") return parseDo();
        if (w == "while") return parseWhile();
        if (w == "with") return parseWith();
        if (w == "sub") return parseProcDecl(true);
        if (w == "function") return parseProcDecl(false);
        if (w == "property") return parsePropDecl();
        if (w == "class") return parseClassDecl();
        if (w == "exit") {
            advance();
            auto s = std::make_unique<Stmt>();
            s->k = Stmt::Exit; s->line = line; s->name = identText();
            if (peek().k != TK::Ident) fail("expected Do, For, Sub, Function or Property");
            advance();
            return s;
        }
        if (w == "end") {
            advance();
            if (peek().k == TK::Ident) fail("unexpected 'End " + lower(peek().text) + "'");
            auto s = std::make_unique<Stmt>();
            s->k = Stmt::EndStmt; s->line = line;
            return s;
        }
        if (w == "stop") {
            advance();
            auto s = std::make_unique<Stmt>();
            s->k = Stmt::EndStmt; s->line = line;
            return s;
        }
        if (w == "call") { advance(); return parseCallStmt(); }
        if (w == "set") { advance(); return parseAssign(true); }
        if (w == "let") { advance(); return parseAssign(false); }
        if (w == "on") {
            advance();
            expectIdent("error");
            return parseOnError();
        }
        if (w == "execute") {
            advance();
            auto s = std::make_unique<Stmt>();
            s->k = Stmt::Execute; s->line = line; s->e = parseExpr();
            return s;
        }
        if (w == "executeglobal") {
            advance();
            auto s = std::make_unique<Stmt>();
            s->k = Stmt::ExecuteGlobal; s->line = line; s->e = parseExpr();
            return s;
        }
        if (w == "randomize") {
            advance();
            auto s = std::make_unique<Stmt>();
            s->k = Stmt::Randomize; s->line = line;
            if (canStartExpr()) s->e = parseExpr();
            return s;
        }
        if (w == "erase") {
            advance();
            auto s = std::make_unique<Stmt>();
            s->k = Stmt::Erase; s->line = line;
            for (;;) {
                if (peek().k != TK::Ident) fail("expected identifier");
                s->exprs.push_back(Expr::mkVar(advance().text, line));
                if (atOp(",")) { advance(); continue; }
                break;
            }
            return s;
        }
        if (w == "goto") {
            advance();
            auto s = std::make_unique<Stmt>();
            s->k = Stmt::Goto; s->line = line;
            if (peek().k != TK::Ident && peek().k != TK::Number) fail("expected label");
            s->name = lower(advance().text);
            return s;
        }
        // fall through: expression / assignment / call
        return parseExprOrAssign();
    }

    return parseExprOrAssign();
}

StmtPtr Parser::parseExprOrAssign() {
    int line = peek().line;
    ExprPtr head = parsePostfix();
    if (atOp("=")) {
        advance();
        auto s = std::make_unique<Stmt>();
        s->k = Stmt::Assign; s->line = line; s->b = false;
        s->e = std::move(head);
        s->e2 = parseExpr();
        return s;
    }
    if ((head->k == Expr::Var || head->k == Expr::Member) && canStartExpr()) {
        auto s = std::make_unique<Stmt>();
        s->k = Stmt::CallStmt; s->line = line;
        std::vector<ExprPtr> args;
        for (;;) {
            args.push_back(parseExpr());
            if (atOp(",")) { advance(); continue; }
            break;
        }
        s->e = Expr::mkCall(std::move(head), std::move(args), line);
        return s;
    }
    ExprPtr full = parseBinRest(std::move(head), 0);
    auto s = std::make_unique<Stmt>();
    s->k = Stmt::ExprStmt; s->line = line; s->e = std::move(full);
    return s;
}

StmtPtr Parser::parseCallStmt() {
    int line = peek().line;
    ExprPtr head = parsePostfix();
    std::vector<ExprPtr> args;
    if (head->k == Expr::Call) {
        // already has parens
        auto s = std::make_unique<Stmt>();
        s->k = Stmt::CallStmt; s->line = line; s->e = std::move(head);
        return s;
    }
    if (canStartExpr()) {
        for (;;) {
            args.push_back(parseExpr());
            if (atOp(",")) { advance(); continue; }
            break;
        }
    }
    auto s = std::make_unique<Stmt>();
    s->k = Stmt::CallStmt; s->line = line;
    s->e = Expr::mkCall(std::move(head), std::move(args), line);
    return s;
}

StmtPtr Parser::parseAssign(bool isSet) {
    int line = peek().line;
    ExprPtr head = parsePostfix();
    expectOp("=");
    auto s = std::make_unique<Stmt>();
    s->k = Stmt::Assign; s->line = line; s->b = isSet;
    s->e = std::move(head);
    s->e2 = parseExpr();
    return s;
}

StmtPtr Parser::parseIf() {
    int line = peek().line;
    advance(); // if
    auto s = std::make_unique<Stmt>();
    s->k = Stmt::If; s->line = line;
    s->e = parseExpr();
    expectIdent("then");
    if (peek().k == TK::Newline) {
        // block form
        s->body = parseStatements({"elseif", "else", "end"});
        while (atIdent("elseif")) {
            advance();
            ExprPtr c = parseExpr();
            expectIdent("then");
            std::vector<ExprPtr> cv; cv.push_back(std::move(c));
            auto body = parseStatements({"elseif", "else", "end"});
            s->arms.push_back({std::move(cv), std::move(body)});
        }
        if (atIdent("else")) {
            advance();
            s->body2 = parseStatements({"end"});
        }
        expectIdent("end");
        expectIdent("if");
    } else {
        // single-line form
        s->body = parseInlineStatements();
        if (atIdent("else")) {
            advance();
            s->body2 = parseInlineStatements();
        }
    }
    return s;
}

StmtPtr Parser::parseSelect() {
    int line = peek().line;
    advance(); // select
    expectIdent("case");
    auto s = std::make_unique<Stmt>();
    s->k = Stmt::Select; s->line = line;
    s->e = parseExpr();
    for (;;) {
        skipSeparators();
        if (atIdent("end")) { advance(); expectIdent("select"); break; }
        expectIdent("case");
        std::vector<ExprPtr> items;
        bool isElse = false;
        if (atIdent("else")) { advance(); isElse = true; }
        else {
            for (;;) {
                if (atIdent("is")) {
                    advance();
                    std::string op;
                    if (peek().k == TK::Op &&
                        (peek().text == "=" || peek().text == "<>" || peek().text == "<" ||
                         peek().text == ">" || peek().text == "<=" || peek().text == ">=")) {
                        op = advance().text;
                    } else fail("expected comparison operator after Is");
                    ExprPtr rhs = parseExpr();
                    // encode as Bin with a sentinel left child; interpreter compares against case expr
                    auto sentinel = Expr::mkVar("\x01" "case", line);
                    items.push_back(Expr::mkBin(op, std::move(sentinel), std::move(rhs), line));
                } else {
                    ExprPtr lo = parseExpr();
                    if (atIdent("to")) {
                        advance();
                        ExprPtr hi = parseExpr();
                        items.push_back(Expr::mkBin("to", std::move(lo), std::move(hi), line));
                    } else {
                        items.push_back(std::move(lo));
                    }
                }
                if (atOp(",")) { advance(); continue; }
                break;
            }
        }
        (void)isElse;
        auto body = parseStatements({"case", "end"});
        s->arms.push_back({std::move(items), std::move(body)});
    }
    return s;
}

StmtPtr Parser::parseFor() {
    int line = peek().line;
    advance(); // for
    auto s = std::make_unique<Stmt>();
    s->line = line;
    if (atIdent("each")) {
        advance();
        s->k = Stmt::ForEach;
        if (peek().k != TK::Ident) fail("expected identifier");
        s->e = Expr::mkVar(advance().text, line);
        expectIdent("in");
        s->e2 = parseExpr();
        s->body = parseStatements({"next"});
        expectIdent("next");
        if (peek().k == TK::Ident) advance(); // optional var
    } else {
        s->k = Stmt::For;
        if (peek().k != TK::Ident) fail("expected identifier");
        s->e = Expr::mkVar(advance().text, line);
        expectOp("=");
        s->e2 = parseExpr(); // start
        expectIdent("to");
        s->exprs.push_back(parseExpr()); // end -> exprs[0]
        if (atIdent("step")) { advance(); s->e3 = parseExpr(); }
        s->body = parseStatements({"next"});
        expectIdent("next");
        if (peek().k == TK::Ident) advance();
    }
    return s;
}

StmtPtr Parser::parseDo() {
    int line = peek().line;
    advance(); // do
    auto s = std::make_unique<Stmt>();
    s->k = Stmt::DoLoop; s->line = line;
    // pre-condition
    if (atIdent("while")) { advance(); s->name = "while"; s->e = parseExpr(); }
    else if (atIdent("until")) { advance(); s->name = "until"; s->e = parseExpr(); }
    s->body = parseStatements({"loop"});
    expectIdent("loop");
    if (atIdent("while")) { advance(); s->name2 = "while"; s->e2 = parseExpr(); }
    else if (atIdent("until")) { advance(); s->name2 = "until"; s->e2 = parseExpr(); }
    return s;
}

StmtPtr Parser::parseWhile() {
    int line = peek().line;
    advance(); // while
    auto s = std::make_unique<Stmt>();
    s->k = Stmt::While; s->line = line;
    s->e = parseExpr();
    s->body = parseStatements({"wend"});
    expectIdent("wend");
    return s;
}

StmtPtr Parser::parseWith() {
    int line = peek().line;
    advance(); // with
    auto s = std::make_unique<Stmt>();
    s->k = Stmt::With; s->line = line;
    s->e = parseExpr();
    s->body = parseStatements({"end"});
    expectIdent("end");
    expectIdent("with");
    return s;
}

std::vector<Param> Parser::parseParams() {
    std::vector<Param> params;
    if (!atOp("(")) return params;
    advance();
    if (atOp(")")) { advance(); return params; }
    for (;;) {
        Param pm;
        if (atIdent("optional")) { pm.optional = true; advance(); }
        if (atIdent("byval")) { pm.byRef = false; advance(); }
        else if (atIdent("byref")) { pm.byRef = true; advance(); }
        if (peek().k != TK::Ident) fail("expected parameter name");
        pm.name = lower(advance().text);
        if (atOp("(")) { advance(); expectOp(")"); } // array param
        if (atIdent("as")) { advance(); if (peek().k == TK::Ident) advance(); }
        if (atOp("=")) {
            advance();
            pm.hasDefault = true;
            pm.def = parseExpr();
        }
        params.push_back(std::move(pm));
        if (atOp(",")) { advance(); continue; }
        break;
    }
    expectOp(")");
    return params;
}

StmtPtr Parser::parseProcDecl(bool isSub) {
    int line = peek().line;
    advance(); // sub/function
    auto s = std::make_unique<Stmt>();
    s->k = isSub ? Stmt::SubDecl : Stmt::FuncDecl;
    s->line = line;
    if (peek().k != TK::Ident) fail("expected procedure name");
    s->name = lower(advance().text);
    s->params = parseParams();
    if (!isSub && atIdent("as")) { advance(); if (peek().k == TK::Ident) advance(); }
    s->body = parseStatements({"end"});
    expectIdent("end");
    expectIdent(isSub ? "sub" : "function");
    return s;
}

StmtPtr Parser::parsePropDecl() {
    int line = peek().line;
    advance(); // property
    auto s = std::make_unique<Stmt>();
    s->k = Stmt::PropDecl; s->line = line;
    std::string kind = identText();
    if (kind != "get" && kind != "let" && kind != "set") fail("expected Get, Let or Set");
    advance();
    s->op = kind;
    if (peek().k != TK::Ident) fail("expected property name");
    s->name = lower(advance().text);
    s->params = parseParams();
    s->body = parseStatements({"end"});
    expectIdent("end");
    expectIdent("property");
    return s;
}

StmtPtr Parser::parseClassDecl() {
    int line = peek().line;
    advance(); // class
    auto s = std::make_unique<Stmt>();
    s->k = Stmt::ClassDecl; s->line = line;
    if (peek().k != TK::Ident) fail("expected class name");
    s->name = lower(advance().text);
    s->members = parseStatements({"end"});
    expectIdent("end");
    expectIdent("class");
    return s;
}

Decl Parser::parseDecl(bool parensRequired) {
    Decl d;
    if (peek().k != TK::Ident) fail("expected identifier");
    d.name = lower(advance().text);
    if (atOp("(")) {
        advance();
        d.isArray = true;
        if (atOp(")")) { d.dynamic = true; advance(); }
        else {
            d.dynamic = false;
            for (;;) {
                d.dims.push_back(parseExpr());
                if (atOp(",")) { advance(); continue; }
                break;
            }
            expectOp(")");
        }
    } else if (parensRequired) fail("expected '('");
    if (atIdent("as")) {
        advance();
        if (peek().k == TK::Ident) d.asType = lower(advance().text);
    }
    return d;
}

std::vector<Decl> Parser::parseDeclList(bool parensRequired) {
    std::vector<Decl> out;
    for (;;) {
        out.push_back(parseDecl(parensRequired));
        if (atOp(",")) { advance(); continue; }
        break;
    }
    return out;
}

StmtPtr Parser::parseOnError() {
    int line = peek().line;
    auto s = std::make_unique<Stmt>();
    s->k = Stmt::OnError; s->line = line;
    if (atIdent("resume")) {
        advance();
        expectIdent("next");
        s->b = true; // resumeNext
        return s;
    }
    if (atIdent("goto")) {
        advance();
        s->b = false;
        if (peek().k == TK::Number && peek().num == 0) { advance(); s->num = 0; }
        else if (peek().k == TK::Ident) { s->name = lower(advance().text); s->num = -1; }
        else if (peek().k == TK::Number) { s->num = (long long)advance().num; }
        else fail("expected 0 or label");
        return s;
    }
    fail("expected Resume Next or Goto");
}

// ---------- expressions ----------

int Parser::precOf(const std::string& op) const {
    if (op == "^") return 13;
    if (op == "*" || op == "/") return 12;
    if (op == "\\") return 11;
    if (op == "mod") return 10;
    if (op == "+" || op == "-") return 9;
    if (op == "&") return 8;
    if (op == "=" || op == "<>" || op == "<" || op == ">" || op == "<=" || op == ">=" || op == "is") return 7;
    if (op == "not") return 6;
    if (op == "and") return 5;
    if (op == "or") return 4;
    if (op == "xor") return 3;
    if (op == "eqv") return 2;
    if (op == "imp") return 1;
    return -1;
}

bool Parser::canStartExpr() const {
    const Token& tk = peek();
    if (tk.k == TK::Number || tk.k == TK::String || tk.k == TK::Ident) return true;
    if (tk.k == TK::Op && (tk.text == "(" || tk.text == "-" || tk.text == "+")) return true;
    if (tk.k == TK::Op && tk.text == "." && tk.spaceBefore) return true; // With shorthand arg
    return false;
}

ExprPtr Parser::parseExpr(int minPrec) {
    // prefix Not (logical, precedence 6)
    if (atIdent("not")) {
        int prec = 6;
        if (prec >= minPrec) {
            advance();
            ExprPtr operand = parseExpr(prec + 1);
            return Expr::mkUn("not", std::move(operand), peek().line);
        }
    }
    ExprPtr left = parseUnary();
    return parseBinRest(std::move(left), minPrec);
}

ExprPtr Parser::parseBinRest(ExprPtr left, int minPrec) {
    for (;;) {
        std::string op;
        int prec = -1;
        Token& tk = peek();
        if (tk.k == TK::Op) { op = tk.text; prec = precOf(op); }
        else if (tk.k == TK::Ident) {
            std::string w = lower(tk.text);
            int pr = precOf(w);
            if (pr > 0 && w != "not") { op = w; prec = pr; }
            else break;
        } else break;
        if (prec < 0 || prec < minPrec) break;
        advance();
        ExprPtr right = parseExpr(op == "^" ? prec : prec + 1);
        left = Expr::mkBin(op, std::move(left), std::move(right), tk.line);
    }
    return left;
}

ExprPtr Parser::parseUnary() {
    if (atOp("-")) {
        int line = peek().line;
        advance();
        return Expr::mkUn("-", parseUnary(), line);
    }
    if (atOp("+")) { advance(); return parseUnary(); }
    return parsePostfix();
}

ExprPtr Parser::parsePostfix() {
    ExprPtr e = parsePrimary();
    for (;;) {
        if (atOp(".") && !peek().spaceBefore) {
            advance();
            if (peek().k != TK::Ident) fail("expected member name after '.'");
            e = Expr::mkMember(std::move(e), advance().text, peek().line);
            continue;
        }
        if (atOp(".") && peek().spaceBefore) break; // " .member" -> With shorthand in arg position
        if (atOp("!")) {
            advance();
            if (peek().k != TK::Ident) fail("expected member name after '!'");
            e = Expr::mkMember(std::move(e), advance().text, peek().line);
            continue;
        }
        if (atOp("(")) {
            advance();
            std::vector<ExprPtr> args;
            if (!atOp(")")) {
                for (;;) {
                    args.push_back(parseExpr());
                    if (atOp(",")) { advance(); continue; }
                    break;
                }
            }
            expectOp(")");
            e = Expr::mkCall(std::move(e), std::move(args), peek().line);
            continue;
        }
        break;
    }
    return e;
}

std::vector<ExprPtr> Parser::parseArgs() {
    std::vector<ExprPtr> args;
    if (atOp(")")) return args;
    for (;;) {
        args.push_back(parseExpr());
        if (atOp(",")) { advance(); continue; }
        break;
    }
    return args;
}

ExprPtr Parser::parsePrimary() {
    Token& tk = peek();
    int line = tk.line;
    if (tk.k == TK::Number) {
        advance();
        if (tk.isInt) {
            long long v = (long long)tk.num;
            return Expr::mkLit(Variant::mkInt(v), line);
        }
        return Expr::mkLit(Variant::mkDouble(tk.num), line);
    }
    if (tk.k == TK::String) {
        advance();
        return Expr::mkLit(Variant::mkStr(tk.text), line);
    }
    if (tk.k == TK::Op && tk.text == "(") {
        advance();
        ExprPtr e = parseExpr();
        expectOp(")");
        return e;
    }
    if (tk.k == TK::Op && tk.text == ".") {
        // With-block shorthand: .Member
        advance();
        if (peek().k != TK::Ident) fail("expected member name after '.'");
        return Expr::mkMember(Expr::mkVar("\x01" "with", line), advance().text, line);
    }
    if (tk.k == TK::Ident) {
        std::string w = lower(tk.text);
        if (w == "true") { advance(); return Expr::mkLit(Variant::mkBool(true), line); }
        if (w == "false") { advance(); return Expr::mkLit(Variant::mkBool(false), line); }
        if (w == "empty") { advance(); return Expr::mkLit(Variant(), line); }
        if (w == "null") { advance(); return Expr::mkLit(Variant::mkNull(), line); }
        if (w == "nothing") { advance(); return Expr::mkLit(Variant::mkObj(nullptr), line); }
        if (w == "new") {
            advance();
            if (peek().k != TK::Ident) fail("expected class name after New");
            std::string cls = lower(advance().text);
            std::vector<ExprPtr> args;
            if (atOp("(")) { advance(); args = parseArgs(); expectOp(")"); }
            return Expr::mkNew(cls, std::move(args), line);
        }
        advance();
        return Expr::mkVar(tk.text, line);
    }
    fail("expected expression");
}
