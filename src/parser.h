// parser.h — recursive-descent parser producing a Program AST.
#pragma once

#include "ast.h"
#include "lexer.h"
#include <set>
#include <string>
#include <vector>

class Parser {
public:
    explicit Parser(std::vector<Token> toks) : t(std::move(toks)) {}

    Program parseProgram();
    // helpers used by the interpreter for Eval / Execute
    static ExprPtr parseExpressionString(const std::string& src);   // throws ParseError
    static Program parseStatementsString(const std::string& src);    // throws ParseError

private:
    std::vector<Token> t;
    size_t p = 0;

    Token& peek(size_t off = 0);
    const Token& peek(size_t off = 0) const;
    Token& advance();
    bool at(TK k) const;
    bool atOp(const std::string& s) const;
    bool atIdent(const std::string& kw) const; // case-insensitive keyword
    std::string identText() const;              // lowercased ident text of peek
    void expectOp(const std::string& s);
    void expectIdent(const std::string& kw);
    [[noreturn]] void fail(const std::string& msg);
    void skipSeparators(); // newlines and colons

    // statements
    StmtPtr parseStatement();
    std::vector<StmtPtr> parseStatements(const std::set<std::string>& terminators);
    std::vector<StmtPtr> parseInlineStatements(); // until newline/EOF (colon-separated)
    StmtPtr parseIf();
    StmtPtr parseSelect();
    StmtPtr parseFor();
    StmtPtr parseDo();
    StmtPtr parseWhile();
    StmtPtr parseWith();
    StmtPtr parseProcDecl(bool isSub);   // Sub / Function
    StmtPtr parsePropDecl();
    StmtPtr parseClassDecl();
    StmtPtr parseDimLike();              // Dim / Public / Private / Static
    StmtPtr parseReDim();
    StmtPtr parseConst();
    StmtPtr parseOnError();
    StmtPtr parseCallStmt();
    StmtPtr parseAssign(bool isSet);
    StmtPtr parseExprOrAssign();
    std::vector<Param> parseParams();
    std::vector<Decl> parseDeclList(bool parensRequired);
    Decl parseDecl(bool parensRequired);

    // expressions
    ExprPtr parseExpr(int minPrec = 0);
    ExprPtr parseBinRest(ExprPtr left, int minPrec);
    ExprPtr parseUnary();
    ExprPtr parsePostfix();
    ExprPtr parsePrimary();
    std::vector<ExprPtr> parseArgs();    // inside parens
    int precOf(const std::string& op) const;
    bool canStartExpr() const;
};
