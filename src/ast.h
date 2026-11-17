// ast.h — syntax tree nodes for mscript.
#pragma once

#include "variant.h"
#include <memory>
#include <string>
#include <utility>
#include <vector>

struct Expr;
using ExprPtr = std::unique_ptr<Expr>;

struct Expr {
    enum K { Lit, Var, Unary, Bin, Call, Member, NewExpr } k = Lit;
    int line = 0;
    Variant lit;                 // Lit
    std::string name;            // Var, Member, NewExpr (lowercased)
    std::string op;              // Unary/Bin operator
    std::vector<ExprPtr> a;      // children / call args

    static ExprPtr mkLit(Variant v, int line = 0) {
        auto e = std::make_unique<Expr>(); e->k = Lit; e->lit = std::move(v); e->line = line; return e;
    }
    static ExprPtr mkVar(std::string n, int line = 0) {
        auto e = std::make_unique<Expr>(); e->k = Var; e->name = lower(std::move(n)); e->line = line; return e;
    }
    static ExprPtr mkUn(std::string op, ExprPtr x, int line = 0) {
        auto e = std::make_unique<Expr>(); e->k = Unary; e->op = std::move(op); e->line = line;
        e->a.push_back(std::move(x)); return e;
    }
    static ExprPtr mkBin(std::string op, ExprPtr l, ExprPtr r, int line = 0) {
        auto e = std::make_unique<Expr>(); e->k = Bin; e->op = std::move(op); e->line = line;
        e->a.push_back(std::move(l)); e->a.push_back(std::move(r)); return e;
    }
    static ExprPtr mkCall(ExprPtr callee, std::vector<ExprPtr> args, int line = 0) {
        auto e = std::make_unique<Expr>(); e->k = Call; e->line = line;
        e->a.push_back(std::move(callee));
        for (auto& x : args) e->a.push_back(std::move(x));
        return e;
    }
    static ExprPtr mkMember(ExprPtr obj, std::string name, int line = 0) {
        auto e = std::make_unique<Expr>(); e->k = Member; e->name = lower(std::move(name)); e->line = line;
        e->a.push_back(std::move(obj)); return e;
    }
    static ExprPtr mkNew(std::string cls, std::vector<ExprPtr> args, int line = 0) {
        auto e = std::make_unique<Expr>(); e->k = NewExpr; e->name = lower(std::move(cls)); e->line = line;
        for (auto& x : args) e->a.push_back(std::move(x));
        return e;
    }
};

struct Param {
    std::string name;        // lowercased
    bool byRef = true;
    bool optional = false;
    bool hasDefault = false;
    ExprPtr def;
};

struct Decl {                // Dim / ReDim / Public / Private entry
    std::string name;        // lowercased
    bool isArray = false;    // has parens
    bool dynamic = false;    // empty parens: Dim x()
    std::vector<ExprPtr> dims;
    std::string asType;      // optional As clause (informational)
};

struct Stmt;
using StmtPtr = std::unique_ptr<Stmt>;

struct Stmt {
    enum K {
        Nop, ExprStmt, Assign, CallStmt,
        If, Select, For, ForEach, DoLoop, While, With,
        Dim, ReDim, ConstDecl, SubDecl, FuncDecl, PropDecl, ClassDecl,
        OnError, Exit, EndStmt, Goto, Label,
        Option, Randomize, Erase, Execute, ExecuteGlobal
    } k = Nop;
    int line = 0;

    // generic payload
    std::string name, name2, op;       // names / operator / option kind
    Variant lit;
    bool b = false, b2 = false, b3 = false;
    long long num = 0;
    ExprPtr e, e2, e3;
    std::vector<ExprPtr> exprs;                      // case items, erase targets handled via decls
    std::vector<StmtPtr> body, body2, body3;         // if/else; loop body; with body
    std::vector<std::pair<std::vector<ExprPtr>, std::vector<StmtPtr>>> arms; // select case
    std::vector<Param> params;
    std::vector<Decl> decls;
    std::vector<StmtPtr> members;                    // class body
};

struct Program {
    std::vector<StmtPtr> stmts;
};
