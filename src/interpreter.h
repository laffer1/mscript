// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 Lucas Holt. All rights reserved.

// interpreter.h — tree-walking evaluator for mscript.
#pragma once

#include "ast.h"
#include "variant.h"

#include <deque>
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
    bool isPrivate = false;           // declared Private inside a class
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
    std::string defaultProp;          // name of the Default Property Get, if any
};

struct ClassInstance;

// Class_Terminate bookkeeping shared by an Interp and every instance it
// creates, so an instance released after the Interp is gone still finds it.
struct TerminateQueue {
    bool enabled = true;                            // false once the script has shut down
    std::deque<std::shared_ptr<ClassInstance>> pending; // released, Class_Terminate not yet run
};

struct ClassInstance : VBObject {
    Interp* interp = nullptr;
    ClassInfo* cls = nullptr;
    Scope fields;                                   // parent = interp->global
    // Weak: a strong self reference would keep every instance alive forever.
    // Me and method calls take a strong reference for as long as they run.
    std::weak_ptr<ClassInstance> self;
    std::shared_ptr<TerminateQueue> termQueue;
    bool terminated = false;                        // automatic Class_Terminate has been queued

    // Creates an instance whose release queues Class_Terminate (see interpreter.cpp).
    static std::shared_ptr<ClassInstance> create(Interp& ip, ClassInfo& ci);

    Proc* findProc(const std::string& name, int kind) const;
    bool isPrivateMember(const std::string& n) const; // Private field or proc
    bool hasMember(const std::string& n) const override;
    bool hasMethod(const std::string& n) const override;
    bool hasDefault() const override;
    bool hasDefaultSetter() const override;
    Variant getDefault() override;
    std::vector<Variant> enumItems() override;   // For Each via hidden _NewEnum
    Variant invoke(const std::vector<Variant>& args) override;             // obj(i) -> default Get
    void assignInvoke(const std::vector<Variant>& args, const Variant& v) override;
    Variant getMember(const std::string& n) override;
    void setMember(const std::string& n, const Variant& v) override;
    Variant callMember(const std::string& n, std::vector<Variant>& args) override;
    std::string objectKind() const override { return cls ? cls->name : "Object"; }

    // Members with arguments: obj.Name(args) reads a field (indexing it), calls
    // Property Get Name(args) or a Sub/Function; obj.Name(args) = v writes a
    // field element or calls Property Let/Set Name(args, v). An empty name
    // means the class's default member.
    bool hasProc(const std::string& n, bool forWrite) const;
    Variant getProp(const std::string& n, std::vector<ArgVal>& args);
    void letProp(const std::string& n, std::vector<Variant>& args, const Variant& v, bool isSet,
                 bool strict = true);
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
    int curLine = 0;                                // for Erl
    std::vector<std::unique_ptr<Variant>> scratch; // temps for with-member lvalues
};

struct ExitSig { std::string kind; };   // do / for / sub / function / property
struct GotoSig { std::string label; };
struct EndSig {};

struct Interp {
    // First member, so it is destroyed last: instances released while the
    // rest of the Interp is torn down check it before queuing anything.
    std::shared_ptr<TerminateQueue> termQueue = std::make_shared<TerminateQueue>();
    int termDrainDepth = 0;

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
    std::shared_ptr<VBObject> globalThis;   // Me at global scope
    bool aspMode = false;                   // running an .asp template
    std::shared_ptr<VBObject> responseObj;  // ASP Response object, if any
    std::vector<std::string> scriptArgs;
    std::string scriptFullName;
    int exitCode = 0;
    bool quitCalled = false;

    std::mt19937_64 rng;
    Frame* cur = nullptr;
    int callDepth = 0; // recursion guard for callProc (error 28 instead of stack overflow)

    Interp();
    ~Interp();
    Interp(const Interp&) = delete;
    Interp& operator=(const Interp&) = delete;
    void load(Program& prog, const std::string& srcName);
    void hoistDecls(Stmt& st); // procs/classes/consts are compile-time
    void run();
    // End of script: release script-level variables, Static locals and the
    // With stack, running Class_Terminate for each object that goes away.
    // Objects kept alive only by a reference cycle are leaked, as in VBScript.
    void shutdown();

    // Class_Terminate: released instances are queued and run here, at the
    // next statement boundary or procedure return.
    void runPendingTerminators() {
        if (!termQueue->pending.empty()) drainTerminators();
    }
    void drainTerminators();
    void runTerminator(ClassInstance& inst, const std::shared_ptr<ClassInstance>& ref);

    // execution
    void execStmtList(const std::vector<Stmt*>& list, Frame& f);
    void execStmt(Stmt& s, Frame& f);
    Variant evalExpr(Expr& e, Frame& f);
    Variant evalMemberBase(Expr& e, Frame& f);
    Variant* evalLValue(Expr& e, Frame& f);
    Variant evalCall(Expr& call, Frame& f);
    ArgVal evalArg(Expr& e, Frame& f);
    void assignTarget(Expr& t, Variant v, bool isSet, Frame& f);
    void checkMemberAccess(const VBObject* obj, const std::string& n, Frame& f);
    Variant callProc(Proc& p, std::vector<ArgVal>& args, std::shared_ptr<ClassInstance> self);
    Variant callProcBody(Proc& p, std::vector<ArgVal>& args, std::shared_ptr<ClassInstance> self);
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
