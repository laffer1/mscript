// interpreter.h — tree-walking evaluator for mscript.
#pragma once

#include "ast.h"
#include "variant.h"

#include <functional>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

struct Interp;

struct Builtin {
    int minArgs = 0;
    int maxArgs = 256;
    std::function<Variant(Interp&, std::vector<Variant>&)> fn;
};

struct ArgVal {
    Variant val;
    Variant* ref = nullptr; // non-null when the argument was an lvalue (ByRef)
};

struct Scope {
    std::unordered_map<std::string, Variant> vars;
    Scope* parent = nullptr;
    Variant* find(const std::string& name) {
        for (Scope* s = this; s; s = s->parent) {
            auto it = s->vars.find(name);
            if (it != s->vars.end()) return &it->second;
        }
        return nullptr;
    }
    Variant& operator[](const std::string& name) { return vars[name]; }
};

struct Proc {
    std::string name;                 // lowercased
    bool isSub = true;
    int propKind = 0;                 // 0 normal, 1 get, 2 let, 3 set
    const std::vector<Param>* params = nullptr; // owned by the AST
    std::vector<Stmt*> body;
    std::string className;            // non-empty for class members
    std::map<std::string, Variant> statics; // Static declarations persist here
};

struct ClassInfo {
    std::string name;
    std::vector<Decl*> fields;        // owned by the AST
    std::vector<Proc*> procs;
};

struct ClassInstance : VBObject {
    Interp* interp = nullptr;
    ClassInfo* cls = nullptr;
    Scope fields;                                   // parent = interp->global
    std::shared_ptr<ClassInstance> self;

    Proc* findProc(const std::string& name, int kind) const;
    bool hasMember(const std::string& n) const override;
    bool hasMethod(const std::string& n) const override;
    Variant getMember(const std::string& n) override;
    void setMember(const std::string& n, const Variant& v) override;
    Variant callMember(const std::string& n, std::vector<Variant>& args) override;
    std::string objectKind() const override { return cls ? cls->name : "Object"; }
};

struct Frame {
    std::unique_ptr<Scope> local;
    Scope* scope = nullptr;
    Proc* proc = nullptr;
    std::shared_ptr<ClassInstance> self;
    Variant ret;
    bool resumeNext = false;
    std::string errLabel;
    bool hasErrLabel = false;
    const std::vector<Stmt*>* bodyList = nullptr;
    std::map<std::string, size_t> labelMap;
    std::vector<std::unique_ptr<Variant>> scratch; // temps for with-member lvalues
};

struct ExitSig { std::string kind; };   // do / for / sub / function / property
struct GotoSig { std::string label; };
struct EndSig {};

struct Interp {
    Program* prog = nullptr;
    std::string sourceName;
    Scope global;
    std::vector<Stmt*> topLevel;

    std::map<std::string, Proc*> procs;        // global procedures (lower name)
    std::map<std::string, ClassInfo> classes;  // lower name
    std::map<std::string, Builtin> builtins;
    std::map<std::string, Variant> constants;

    std::vector<std::unique_ptr<Proc>> procStore;
    std::vector<std::unique_ptr<Program>> ownedPrograms;

    std::vector<Variant> withStack;
    bool optionExplicit = false;
    int optionBase = 0;
    bool compareText = false;

    std::shared_ptr<VBObject> errObj;
    std::shared_ptr<VBObject> wscriptObj;
    std::vector<std::string> scriptArgs;
    std::string scriptFullName;
    int exitCode = 0;
    bool quitCalled = false;

    std::mt19937_64 rng;
    Frame* cur = nullptr;

    Interp();
    void load(Program& prog, const std::string& srcName);
    void run();

    // execution
    void execStmtList(const std::vector<Stmt*>& list, Frame& f);
    void execStmt(Stmt& s, Frame& f);
    Variant evalExpr(Expr& e, Frame& f);
    Variant evalMemberBase(Expr& e, Frame& f);
    Variant* evalLValue(Expr& e, Frame& f);
    Variant evalCall(Expr& call, Frame& f);
    ArgVal evalArg(Expr& e, Frame& f);
    void assignTarget(Expr& t, const Variant& v, bool isSet, Frame& f);
    Variant callProc(Proc& p, std::vector<ArgVal>& args, std::shared_ptr<ClassInstance> self);
    Variant callByName(const std::string& lowerName, std::vector<ArgVal>& args);
    Variant instantiateClass(const std::string& name, std::vector<Expr*>& argExprs, Frame& f, int line);
    Variant* findVar(Frame& f, const std::string& name);

    // registration
    Proc* registerProc(Stmt& s, const std::string& className);
    void registerClass(Stmt& s);

    // builtins / host interop
    Scope& curScope();
    Variant evalString(const std::string& code);
    void executeString(const std::string& code, bool globalScope);
    void seedRng(double s);
    double rnd();
    void setErr(const VBSError& e);
    void printOut(const std::string& s); // host output (Echo / MsgBox)
};

// implemented in builtins.cpp / objects.cpp
void registerBuiltins(Interp& interp);
void registerConstants(Interp& interp);
void registerHostObjects(Interp& interp);
std::shared_ptr<VBObject> createHostObject(Interp& interp, const std::string& progId);
