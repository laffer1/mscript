// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 Lucas Holt. All rights reserved.

#include "interpreter.h"

#include "builtins.h"
#include "objects.h"
#include "parser.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>

static std::vector<Stmt*> ptrVec(std::vector<StmtPtr>& v) {
    std::vector<Stmt*> out;
    out.reserve(v.size());
    for (auto& x : v) out.push_back(x.get());
    return out;
}

// ---------- ClassInstance ----------

Proc* ClassInstance::findProc(const std::string& name, int kind) const {
    if (!cls) return nullptr;
    for (Proc* p : cls->procs)
        if (p->name == name && (kind == 0 ? p->propKind == 0 : p->propKind == kind)) return p;
    return nullptr;
}

bool ClassInstance::hasMember(const std::string& n) const {
    if (fields.vars.count(n)) return true;
    if (!cls) return false;
    for (Proc* p : cls->procs) if (p->name == n) return true;
    return false;
}

bool ClassInstance::hasDefault() const {
    return cls && !cls->defaultProp.empty();
}

std::vector<Variant> ClassInstance::enumItems() {
    // For Each over a class instance uses a hidden _NewEnum function that
    // returns an array (the IEnumVARIANT stand-in)
    if (cls) {
        if (Proc* p = findProc("_newenum", 0)) {
            std::vector<ArgVal> noargs;
            Variant v = interp->callProc(*p, noargs, self.lock());
            if (v.t == VT::Array && v.arr && v.arr->allocated)
                return std::vector<Variant>(v.arr->data.begin(), v.arr->data.end());
        }
    }
    return VBObject::enumItems();
}

Variant ClassInstance::getDefault() {
    return getMember("");
}

bool ClassInstance::hasMethod(const std::string& n) const {
    if (!cls) return false;
    for (Proc* p : cls->procs) if (p->name == n && p->propKind == 0) return true;
    return false;
}

// arr(i, j) or obj(args) applied to a member's value; no arguments = the value itself
static Variant indexValue(const Variant& v, const std::vector<Variant>& vals) {
    if (vals.empty()) return v;
    if (v.t == VT::Array && v.arr) {
        std::vector<long long> idx;
        for (auto& x : vals) idx.push_back(toInt64(x));
        return v.arr->at(idx);
    }
    if (v.t == VT::Object && v.obj) return v.obj->invoke(vals);
    throwErr(450, errDesc(450));
}

static std::vector<Variant> argValues(const std::vector<ArgVal>& args) {
    std::vector<Variant> vals;
    for (auto& a : args) vals.push_back(a.val);
    return vals;
}

// Parameters a call must supply: all of them, less trailing Optional ones.
static size_t requiredParams(const Proc& p) {
    size_t n = p.params->size();
    while (n > 0 && ((*p.params)[n - 1].optional || (*p.params)[n - 1].hasDefault)) n--;
    return n;
}

bool ClassInstance::hasProc(const std::string& n, bool forWrite) const {
    if (!cls) return false;
    for (Proc* p : cls->procs) {
        if (p->name != n) continue;
        if (forWrite ? (p->propKind == 2 || p->propKind == 3) : (p->propKind == 0 || p->propKind == 1))
            return true;
    }
    return false;
}

Variant ClassInstance::getProp(const std::string& n0, std::vector<ArgVal>& args) {
    std::string n = n0;
    if (n.empty()) {
        if (!cls || cls->defaultProp.empty()) throwErr(438, errDesc(438));
        n = cls->defaultProp;
    }
    auto it = fields.vars.find(n);
    if (it != fields.vars.end()) return indexValue(it->second, argValues(args));
    Proc* p = findProc(n, 1); // Property Get
    if (!p) p = findProc(n, 0); // Sub / Function
    if (!p) throwErr(438, errDesc(438));
    std::shared_ptr<ClassInstance> me = self.lock();
    if (!args.empty() && p->params->empty() && p->propKind == 1) {
        // Property Get Items() returning an array or a collection: index the result
        std::vector<ArgVal> noargs;
        Variant v = interp->callProc(*p, noargs, me);
        return indexValue(v, argValues(args));
    }
    if (args.size() > p->params->size() || args.size() < requiredParams(*p)) throwErr(450, errDesc(450));
    return interp->callProc(*p, args, me);
}

void ClassInstance::letProp(const std::string& n0, std::vector<Variant>& args, const Variant& v, bool isSet,
                            bool strict) {
    std::string n = n0;
    if (n.empty()) {
        if (!cls || cls->defaultProp.empty()) throwErr(438, errDesc(438));
        n = cls->defaultProp;
    }
    auto it = fields.vars.find(n);
    if (it != fields.vars.end()) {
        Variant& cell = it->second;
        if (args.empty()) { cell = v; ownArrays(cell); return; }
        if (cell.t == VT::Array && cell.arr) {
            std::vector<long long> idx;
            for (auto& x : args) idx.push_back(toInt64(x));
            Variant& slot = cell.arr->at(idx);
            slot = v;
            ownArrays(slot);
            return;
        }
        if (cell.t == VT::Object && cell.obj) { cell.obj->assignInvoke(args, v); return; }
        throwErr(450, errDesc(450));
    }
    // Set uses Property Set and Let uses Property Let (no fallback: Set on a
    // Let-only property is 438, as in VBScript). Host-side callers that don't
    // know which statement it was (strict == false) take Let, then Set.
    Proc* p = findProc(n, isSet ? 3 : 2);
    if (!p && !strict) p = findProc(n, isSet ? 2 : 3);
    std::shared_ptr<ClassInstance> me = self.lock();
    if (p && p->params->size() == args.size() + 1) {
        // the last parameter receives the value; the others are the indexes
        std::vector<ArgVal> av(p->params->size());
        for (size_t i = 0; i < args.size(); i++) av[i].val = args[i];
        av.back().val = v;
        interp->callProc(*p, av, me);
        return;
    }
    if (!args.empty()) {
        // obj.Items(k) = v where Property Get Items() returns a collection
        if (Proc* g = findProc(n, 1); g && g->params->empty()) {
            std::vector<ArgVal> noargs;
            Variant base = interp->callProc(*g, noargs, me);
            if (base.t == VT::Object && base.obj) { base.obj->assignInvoke(args, v); return; }
            if (base.t == VT::Array && base.arr) {
                std::vector<long long> idx;
                for (auto& x : args) idx.push_back(toInt64(x));
                base.arr->at(idx) = v; // a copy of the array, as in VBScript
                return;
            }
        }
    }
    if (p) throwErr(450, errDesc(450));
    throwErr(438, errDesc(438));
}

Variant ClassInstance::getMember(const std::string& n) {
    std::vector<ArgVal> noargs;
    return getProp(n, noargs);
}

void ClassInstance::setMember(const std::string& n, const Variant& v) {
    std::vector<Variant> noargs;
    letProp(n, noargs, v, false, false);
}

Variant ClassInstance::callMember(const std::string& n, std::vector<Variant>& args) {
    std::vector<ArgVal> av;
    for (auto& a : args) { ArgVal x; x.val = a; av.push_back(x); }
    return getProp(n, av);
}

Variant ClassInstance::invoke(const std::vector<Variant>& args) {
    std::vector<ArgVal> av;
    for (auto& a : args) { ArgVal x; x.val = a; av.push_back(x); }
    return getProp("", av);
}

void ClassInstance::assignInvoke(const std::vector<Variant>& args, const Variant& v) {
    std::vector<Variant> a = args;
    letProp("", a, v, false, false);
}

// ---------- Class_Terminate ----------
//
// VBScript objects are reference counted, and Class_Terminate runs once, when
// the last reference goes: Set x = Nothing, reassignment, a local going out of
// scope at End Sub/Function, an array element or Dictionary item being
// dropped, or the end of the script. Instances are owned by shared_ptrs whose
// deleter is releaseClassInstance(). When the count reaches zero it does not
// free the instance but hands it, under a fresh shared_ptr, to the Interp's
// queue; the interpreter runs the queue at the next statement boundary or
// procedure return, where running script code is safe. Once Class_Terminate
// has run, the queue's reference is dropped and the instance is freed (or
// kept, if Class_Terminate stored Me somewhere; it is not terminated again).

// Freeing an instance frees its fields, which can free the next instance in a
// linked list, and so on; doing that recursively overflows the C++ stack on
// long lists. Instances released while one is being freed are collected here
// and freed one after another instead.
static std::vector<ClassInstance*>* g_freeing = nullptr;

static void freeClassInstance(ClassInstance* p) {
    if (g_freeing) {
        try { g_freeing->push_back(p); return; } catch (...) {}
        delete p; // out of memory: fall back to freeing it right away
        return;
    }
    std::vector<ClassInstance*> list;
    g_freeing = &list;
    struct Reset { ~Reset() { g_freeing = nullptr; } } reset;
    delete p;
    while (!list.empty()) {
        ClassInstance* x = list.back();
        list.pop_back();
        delete x;
    }
}

static void releaseClassInstance(ClassInstance* p) {
    try {
        const auto& q = p->termQueue;
        if (!p->terminated && q && q->enabled && p->findProc("class_terminate", 0)) {
            p->terminated = true;
            std::shared_ptr<ClassInstance> again(p, freeClassInstance); // frees p if this throws
            p->self = again;
            q->pending.push_back(std::move(again));
            return;
        }
    } catch (...) {
        return; // out of memory while queuing: p is already freed, skip Class_Terminate
    }
    freeClassInstance(p);
}

std::shared_ptr<ClassInstance> ClassInstance::create(Interp& ip, ClassInfo& ci) {
    std::shared_ptr<ClassInstance> inst(new ClassInstance, releaseClassInstance);
    inst->interp = &ip;
    inst->cls = &ci;
    inst->fields.parent = &ip.global;
    inst->self = inst;
    inst->termQueue = ip.termQueue;
    return inst;
}

void Interp::drainTerminators() {
    // Terminators may release more objects; nesting runs them in the same
    // order VBScript would, but deep chains are left to the outermost loop.
    constexpr int MAX_NESTED_DRAIN = 32;
    if (termDrainDepth >= MAX_NESTED_DRAIN) return;
    ++termDrainDepth;
    struct Guard { Interp& i; ~Guard() { i.termDrainDepth--; } } guard { *this };
    auto& pending = termQueue->pending;
    while (!pending.empty()) {
        std::shared_ptr<ClassInstance> inst = std::move(pending.front());
        pending.pop_front();
        runTerminator(*inst, inst);
        // inst goes out of scope here: its fields are released and may queue more
    }
}

void Interp::runTerminator(ClassInstance& inst, const std::shared_ptr<ClassInstance>& ref) {
    Proc* p = inst.findProc("class_terminate", 0);
    if (!p) return;
    // Errors raised in Class_Terminate are discarded, and the implicit call
    // leaves the Err object as the interrupted code had it.
    auto* err = static_cast<MapObject*>(errObj.get());
    std::map<std::string, Variant> savedErr;
    if (err) savedErr = err->props;
    struct ErrRestore {
        MapObject* e; std::map<std::string, Variant>& saved;
        ~ErrRestore() { if (e) e->props = std::move(saved); }
    } restore { err, savedErr };
    try {
        std::vector<ArgVal> noargs;
        callProc(*p, noargs, ref);
    } catch (VBSError&) {
    } catch (ExitSig&) {
    } catch (GotoSig&) {
    }
    // EndSig (WScript.Quit inside Class_Terminate) propagates and ends the script.
}

void Interp::shutdown() {
    // Script-level variables are released in declaration order, then any
    // others (implicitly declared) by name; VBScript does not document an
    // order. Each variable is set to Empty, not removed, so a Class_Terminate
    // that reads or writes globals still finds them. A terminator may store a
    // new object in a global, so this repeats a few times before giving up.
    std::vector<std::string> order;
    for (Stmt* st : topLevel)
        if (st->k == Stmt::Dim)
            for (auto& d : st->decls) order.push_back(d.name);
    auto releaseAll = [&]() -> bool {
        bool any = false;
        auto clearVar = [&](Variant& v) {
            if (v.t == VT::Object || v.t == VT::Array) {
                Variant old = std::move(v);
                v = Variant();
                any = true;
                // old released here
            }
        };
        std::vector<std::string> names = order;
        std::vector<std::string> rest;
        for (auto& kv : global.vars) rest.push_back(kv.first);
        std::sort(rest.begin(), rest.end());
        names.insert(names.end(), rest.begin(), rest.end());
        for (auto& n : names) {
            auto it = global.vars.find(n);
            if (it == global.vars.end()) continue;
            clearVar(it->second);
            runPendingTerminators();
        }
        for (auto& ps : procStore) {
            for (auto& kv : ps->statics) clearVar(kv.second);
            runPendingTerminators();
        }
        while (!withStack.empty()) { withStack.pop_back(); any = true; }
        runPendingTerminators();
        return any;
    };
    for (int pass = 0; pass < 4; pass++) {
        try {
            if (!releaseAll()) break;
        } catch (EndSig&) {
            // WScript.Quit inside a Class_Terminate: keep releasing
        }
    }
    // Anything released from here on (cycles broken while the Interp is torn
    // down) is freed without running script code.
    termQueue->enabled = false;
    termQueue->pending.clear();
}

// ---------- Interp ----------

Interp::~Interp() {
    termQueue->enabled = false;
    termQueue->pending.clear();
}

Interp::Interp() {
    global.parent = nullptr;
    rng.seed((uint64_t)std::chrono::high_resolution_clock::now().time_since_epoch().count());
    registerConstants(*this);
    registerBuiltins(*this);
    registerHostObjects(*this);
}

Scope& Interp::curScope() { return cur ? *cur->scope : global; }

void Interp::printOut(const std::string& s) { std::fwrite(s.data(), 1, s.size(), stdout); }

void Interp::seedRng(double s) {
    // VB Randomize semantics: seed from number
    uint64_t x = (uint64_t)(s < 0 ? -s : s);
    rng.seed(x ? x : 1);
}

double Interp::rnd() {
    // [0, 1)
    return (rng() >> 11) * (1.0 / 9007199254740992.0);
}

void Interp::setErr(const VBSError& e) {
    if (!errObj) return;
    auto* m = static_cast<MapObject*>(errObj.get());
    m->props["number"] = Variant::mkLong(e.number);
    m->props["description"] = Variant::mkStr(e.what());
    m->props["source"] = Variant::mkStr(e.source);
    m->props["helpfile"] = Variant::mkStr(e.helpfile);
    m->props["helpcontext"] = Variant::mkLong(e.helpcontext);
}

// ---------- registration ----------

Proc* Interp::registerProc(Stmt& s, const std::string& className) {
    auto p = std::make_unique<Proc>();
    p->name = s.name;
    p->isSub = s.k == Stmt::SubDecl;
    p->propKind = 0;
    if (s.k == Stmt::PropDecl) {
        p->isSub = false;
        p->propKind = s.op == "get" ? 1 : (s.op == "let" ? 2 : 3);
    }
    p->params = &s.params;
    for (auto& st : s.body) p->body.push_back(st.get());
    p->className = className;
    Proc* raw = p.get();
    procStore.push_back(std::move(p));
    if (className.empty()) procs[s.name] = raw;
    return raw;
}

void Interp::registerClass(Stmt& s) {
    ClassInfo ci;
    ci.name = s.name;
    for (auto& m : s.members) {
        if (m->k == Stmt::Dim) {
            for (auto& d : m->decls) ci.fields.push_back(&d);
        } else if (m->k == Stmt::SubDecl || m->k == Stmt::FuncDecl || m->k == Stmt::PropDecl) {
            if ((m->k == Stmt::PropDecl || m->k == Stmt::FuncDecl) && m->b)
                ci.defaultProp = m->name; // Default Property / Default Function
            ci.procs.push_back(registerProc(*m, s.name));
        }
    }
    classes[s.name] = std::move(ci);
}

// Procedure, class and const declarations are compile-time constructs in
// VBScript: they are visible everywhere, even inside blocks that never execute.
void Interp::hoistDecls(Stmt& st) {
    switch (st.k) {
        case Stmt::SubDecl: case Stmt::FuncDecl: case Stmt::PropDecl:
            registerProc(st, "");
            return;
        case Stmt::ClassDecl:
            registerClass(st);
            return;
        case Stmt::ConstDecl: {
            Frame f;
            f.scope = &global;
            for (size_t i = 0; i < st.decls.size(); i++)
                constants[st.decls[i].name] = evalExpr(*st.exprs[i], f);
            return;
        }
        default: break;
    }
    for (auto& sub : st.body) hoistDecls(*sub);
    for (auto& sub : st.body2) hoistDecls(*sub);
    for (auto& sub : st.body3) hoistDecls(*sub);
    for (auto& sub : st.members) hoistDecls(*sub);
    for (auto& arm : st.arms)
        for (auto& sub : arm.second) hoistDecls(*sub);
}

void Interp::load(Program& p, const std::string& srcName) {
    prog = &p;
    sourceName = srcName;
    for (auto& st : p.stmts) {
        topLevel.push_back(st.get());
        // top-level Dim names exist (as Empty) from the start of the script
        if (st->k == Stmt::Dim)
            for (auto& d : st->decls)
                global.vars.try_emplace(d.name, Variant()); // Empty until the Dim runs
        hoistDecls(*st);
    }
}

void Interp::run() {
    Frame f;
    f.scope = &global;
    f.bodyList = &topLevel;
    for (size_t i = 0; i < topLevel.size(); i++)
        if (topLevel[i]->k == Stmt::Label) f.labelMap[topLevel[i]->name] = i;
    cur = &f;
    try {
        execStmtList(topLevel, f);
    } catch (EndSig&) {
        // End statement
    }
}

// ---------- variable resolution ----------

Variant* Interp::findVar(Frame& f, const std::string& name) {
    if (Variant* v = f.scope->find(name)) return v;
    // Static declarations of the current procedure
    if (f.proc) {
        auto it = f.proc->statics.find(name);
        if (it != f.proc->statics.end()) return &it->second;
    }
    // With stack (innermost first); a Nothing entry is a barrier from a
    // failed With under Resume Next — do not fall through to outer blocks
    for (auto it = withStack.rbegin(); it != withStack.rend(); ++it) {
        if (it->t == VT::Object && !it->obj) break;
        if (it->t == VT::Object && it->obj && it->obj->hasMember(name)) {
            auto tmp = std::make_unique<Variant>(it->obj->getMember(name));
            Variant* raw = tmp.get();
            f.scratch.push_back(std::move(tmp));
            return raw;
        }
    }
    return nullptr;
}

// Inside a class's procedure, an unqualified name that is not a local or a
// field refers to the class's own Sub, Function or Property before anything
// at script level. Returns the instance to call it on, or null.
static ClassInstance* classMemberFor(Frame& f, const std::string& n, bool forWrite) {
    ClassInstance* me = f.self.get();
    if (!me || !me->hasProc(n, forWrite)) return nullptr;
    if (f.scope->vars.count(n) || me->fields.vars.count(n)) return nullptr; // locals, return slot, fields
    if (f.proc) {
        auto it = f.proc->statics.find(n);
        if (it != f.proc->statics.end()) return nullptr;
    }
    return me;
}

// ---------- statements ----------

static void buildLabels(const std::vector<Stmt*>& list, std::map<std::string, size_t>& m) {
    for (size_t i = 0; i < list.size(); i++)
        if (list[i]->k == Stmt::Label) m[list[i]->name] = i;
}

void Interp::execStmtList(const std::vector<Stmt*>& list, Frame& f) {
    size_t i = 0;
    while (i < list.size()) {
        runPendingTerminators(); // objects released by the previous statement
        Stmt& s = *list[i];
        f.curLine = s.line;
        try {
            execStmt(s, f);
            i++;
        } catch (GotoSig& g) {
            if (&list == f.bodyList) {
                auto it = f.labelMap.find(g.label);
                if (it == f.labelMap.end()) {
                    VBSError e(5, "Label not defined: " + g.label);
                    e.line = s.line;
                    throw e;
                }
                i = it->second;
            } else {
                throw; // propagate up to the procedure body list
            }
        } catch (VBSError& e) {
            if (e.line == 0) e.line = s.line;
            if (f.resumeNext) {
                setErr(e);
                i++;
                continue;
            }
            if (f.hasErrLabel && &list == f.bodyList) {
                auto it = f.labelMap.find(f.errLabel);
                if (it != f.labelMap.end()) {
                    setErr(e);
                    i = it->second;
                    continue;
                }
            }
            throw;
        }
    }
    runPendingTerminators();
}

static Variant numToVar(double d) {
    if (d == std::floor(d) && std::fabs(d) < 9.0e18) return Variant::mkInt((long long)d);
    return Variant::mkDouble(d);
}

void Interp::execStmt(Stmt& s, Frame& f) {
    switch (s.k) {
    case Stmt::Nop:
    case Stmt::Label:
        break;

    case Stmt::ExprStmt: {
        if (s.e->k == Expr::Var) {
            const std::string& n = s.e->name;
            if (ClassInstance* me = classMemberFor(f, n, false)) {
                std::vector<ArgVal> noargs;
                me->getProp(n, noargs);
                break;
            }
            if (!findVar(f, n)) {
                if (procs.count(n)) {
                    std::vector<ArgVal> noargs;
                    callProc(*procs[n], noargs, nullptr);
                    break;
                }
                if (builtins.count(n)) {
                    std::vector<Variant> noargs;
                    builtins[n].fn(*this, noargs);
                    break;
                }
            }
        } else if (s.e->k == Expr::Member) {
            Expr& m = *s.e;
            Variant base = evalMemberBase(*m.a[0], f);
            if (base.t == VT::Object && base.obj && base.obj->hasMethod(m.name)) {
                std::vector<Variant> noargs;
                base.obj->callMember(m.name, noargs);
                break;
            }
        }
        evalExpr(*s.e, f);
        break;
    }

    case Stmt::CallStmt:
        evalExpr(*s.e, f);
        break;

    case Stmt::Assign: {
        Variant v = evalExpr(*s.e2, f);
        assignTarget(*s.e, std::move(v), s.b, f); // moved so a temporary array isn't copied
        break;
    }

    case Stmt::If: {
        // VBScript Resume Next quirk: an error in an If/ElseIf condition
        // resumes execution in that branch's body
        try {
            if (toBool(evalExpr(*s.e, f))) {
                execStmtList(ptrVec(s.body), f);
                break;
            }
        } catch (VBSError& e) {
            if (!f.resumeNext) throw;
            setErr(e);
            execStmtList(ptrVec(s.body), f);
            break;
        }
        for (size_t ai = 0; ai < s.arms.size(); ai++) {
            auto& arm = s.arms[ai];
            try {
                if (toBool(evalExpr(*arm.first[0], f))) {
                    execStmtList(ptrVec(arm.second), f);
                    return;
                }
            } catch (VBSError& e) {
                // an error in an ElseIf condition belongs to the ElseIf line
                if (e.line == 0 && ai < s.armLines.size()) e.line = s.armLines[ai];
                if (!f.resumeNext) throw;
                setErr(e);
                execStmtList(ptrVec(arm.second), f);
                return;
            }
        }
        if (!s.body2.empty()) execStmtList(ptrVec(s.body2), f);
        break;
    }

    case Stmt::Select: {
        Variant cv = evalExpr(*s.e, f);
        for (size_t ai = 0; ai < s.arms.size(); ai++) {
            auto& arm = s.arms[ai];
            if (arm.first.empty()) { // Case Else
                std::vector<Stmt*> v; for (auto& x : arm.second) v.push_back(x.get());
                execStmtList(v, f);
                return;
            }
            bool matched = false;
            for (auto& item : arm.first) {
                bool m;
                try {
                    if (item->k == Expr::Bin && item->op == "to") {
                        Variant lo = evalExpr(*item->a[0], f);
                        Variant hi = evalExpr(*item->a[1], f);
                        Variant c1 = opCompare(">=", cv, lo, compareText);
                        Variant c2 = opCompare("<=", cv, hi, compareText);
                        m = c1.t != VT::Null && c2.t != VT::Null && toBool(c1) && toBool(c2);
                    } else if (item->k == Expr::Bin && item->a[0]->k == Expr::Var && item->a[0]->name == "\x01" "case") {
                        Variant r = evalExpr(*item->a[1], f);
                        Variant c = opCompare(item->op, cv, r, compareText);
                        m = c.t == VT::Null ? false : toBool(c);
                    } else {
                        Variant iv = evalExpr(*item, f);
                        Variant c = opCompare("=", cv, iv, compareText);
                        m = c.t == VT::Null ? false : toBool(c);
                    }
                } catch (VBSError& e) {
                    // an error in a Case expression belongs to the Case line;
                    // under Resume Next it runs that case
                    if (e.line == 0 && ai < s.armLines.size()) e.line = s.armLines[ai];
                    if (!f.resumeNext) throw;
                    setErr(e);
                    execStmtList(ptrVec(arm.second), f);
                    return;
                }
                if (m) { matched = true; break; }
            }
            if (matched) {
                execStmtList(ptrVec(arm.second), f);
                return;
            }
        }
        break;
    }

    case Stmt::For: {
        const std::string& vn = s.e->name;
        Variant* cell = findVar(f, vn);
        if (!cell) {
            if (optionExplicit) throwErr(500, errDesc(500));
            cell = &f.scope->vars[vn];
        }
        // Resume Next: an error in a For header runs the body once, unchanged
        bool headerErr = false;
        double start = 0, end = 0, step = 1;
        try {
            start = toDouble(evalExpr(*s.e2, f));
            end = toDouble(evalExpr(*s.exprs[0], f));
            step = s.e3 ? toDouble(evalExpr(*s.e3, f)) : 1.0;
        } catch (VBSError& e) {
            if (!f.resumeNext) throw;
            if (e.line == 0) e.line = s.line;
            setErr(e);
            headerErr = true;
        }
        if (step == 0 && !headerErr) throwErr(5, errDesc(5));
        if (!headerErr) *cell = numToVar(start);
        auto body = ptrVec(s.body);
        if (headerErr) {
            try {
                execStmtList(body, f);
            } catch (ExitSig& x) {
                if (x.kind != "for") throw;
            }
            break;
        }
        while (step > 0 ? toDouble(*cell) <= end : toDouble(*cell) >= end) {
            try {
                execStmtList(body, f);
            } catch (ExitSig& x) {
                if (x.kind == "for") break;
                throw;
            }
            *cell = numToVar(toDouble(*cell) + step);
        }
        break;
    }

    case Stmt::ForEach: {
        const std::string& vn = s.e->name;
        Variant* cell = findVar(f, vn);
        if (!cell) {
            if (optionExplicit) throwErr(500, errDesc(500));
            cell = &f.scope->vars[vn];
        }
        // Resume Next: an error in a For Each collection expression runs the
        // body once with the loop variable unchanged
        std::vector<Variant> items;
        bool headerErr = false;
        try {
            Variant coll = evalExpr(*s.e2, f);
            if (coll.t == VT::Array) {
                if (!coll.arr || !coll.arr->allocated) throwErr(9, errDesc(9));
                items.assign(coll.arr->data.begin(), coll.arr->data.end());
            } else if (coll.t == VT::Object) {
                if (!coll.obj) throwErr(424, errDesc(424));
                items = coll.obj->enumItems();
            } else {
                throwErr(424, errDesc(424));
            }
        } catch (VBSError& e) {
            if (!f.resumeNext) throw;
            if (e.line == 0) e.line = s.line;
            setErr(e);
            headerErr = true;
        }
        auto body = ptrVec(s.body);
        if (headerErr) {
            try {
                execStmtList(body, f);
            } catch (ExitSig& x) {
                if (x.kind != "for") throw;
            }
            break;
        }
        for (auto& item : items) {
            *cell = item;
            ownArrays(*cell); // the loop variable gets a copy of a nested array
            try {
                execStmtList(body, f);
            } catch (ExitSig& x) {
                if (x.kind == "for") break;
                throw;
            }
        }
        *cell = Variant(); // after For Each the loop variable is Empty
        break;
    }

    case Stmt::DoLoop: {
        auto body = ptrVec(s.body);
        for (;;) {
            if (s.name == "while" || s.name == "until") {
                // Resume Next: an error in a pre-test condition runs the body once
                try {
                    bool c = toBool(evalExpr(*s.e, f));
                    if (s.name == "while" ? !c : c) break;
                } catch (VBSError& e) {
                    if (!f.resumeNext) throw;
                    setErr(e);
                    try {
                        execStmtList(body, f);
                    } catch (ExitSig& x) {
                        if (x.kind != "do") throw;
                    }
                    break;
                }
            }
            try {
                execStmtList(body, f);
            } catch (ExitSig& x) {
                if (x.kind == "do") break;
                throw;
            }
            if (s.e2) {
                // Loop While / Loop Until: an error belongs to the Loop line
                bool c;
                try {
                    c = toBool(evalExpr(*s.e2, f));
                } catch (VBSError& e) {
                    if (e.line == 0) e.line = s.line2;
                    throw;
                }
                if (s.name2 == "while" ? !c : c) break;
            }
            // no conditions at all: Do ... Loop is an infinite loop in VBScript
        }
        break;
    }

    case Stmt::While: {
        auto body = ptrVec(s.body);
        for (;;) {
            bool c;
            try {
                c = toBool(evalExpr(*s.e, f));
            } catch (VBSError& e) {
                // Resume Next: an error in a While condition runs the body once
                if (!f.resumeNext) throw;
                setErr(e);
                execStmtList(body, f);
                break;
            }
            if (!c) break;
            execStmtList(body, f);
        }
        break;
    }

    case Stmt::With: {
        Variant v;
        try {
            v = evalExpr(*s.e, f);
        } catch (VBSError& e) {
            // Resume Next: an error in a With expression still runs the body,
            // but a Nothing barrier keeps With-relative names from resolving
            // against an enclosing With block
            if (!f.resumeNext) throw;
            if (e.line == 0) e.line = s.e->line;
            setErr(e);
            withStack.push_back(Variant::mkObj(nullptr));
            try {
                execStmtList(ptrVec(s.body), f);
            } catch (...) {
                withStack.pop_back();
                throw;
            }
            withStack.pop_back();
            break;
        }
        if (v.t != VT::Object || !v.obj) throwErr(424, errDesc(424));
        withStack.push_back(v);
        try {
            execStmtList(ptrVec(s.body), f);
        } catch (...) {
            withStack.pop_back();
            throw;
        }
        withStack.pop_back();
        break;
    }

    case Stmt::Dim: {
        bool isStatic = s.name == "static";
        for (auto& d : s.decls) {
            if (isStatic && f.proc) {
                auto& m = f.proc->statics;
                if (!d.isArray) {
                    if (!m.count(d.name)) m[d.name] = Variant();
                } else {
                    if (!m.count(d.name)) {
                        auto arr = std::make_shared<VArray>();
                        arr->base = optionBase;
                        if (d.dynamic) { arr->allocated = false; }
                        else {
                            std::vector<long long> dims;
                            for (auto& de : d.dims) {
                                long long n = toInt64(evalExpr(*de, f));
                                if (n < 0) throwErr(5, errDesc(5));
                                dims.push_back(n + 1);
                            }
                            arr->alloc(dims, optionBase, false);
                        }
                        m[d.name] = Variant::mkArr(arr);
                    }
                }
            } else {
                Scope& sc = f.scope ? *f.scope : global;
                if (!d.isArray) {
                    sc.vars[d.name] = Variant();
                } else {
                    auto arr = std::make_shared<VArray>();
                    arr->base = optionBase;
                    if (d.dynamic) {
                        arr->allocated = false;
                    } else {
                        std::vector<long long> dims;
                        for (auto& de : d.dims) {
                            long long n = toInt64(evalExpr(*de, f));
                            if (n < 0) throwErr(5, errDesc(5));
                            dims.push_back(n + 1);
                        }
                        arr->alloc(dims, optionBase, false);
                        arr->fixed = true;
                    }
                    sc.vars[d.name] = Variant::mkArr(arr);
                }
            }
        }
        break;
    }

    case Stmt::ReDim: {
        for (auto& d : s.decls) {
            // ReDim also declares: an unknown name becomes a new variable in
            // the current scope, and a variable holding a non-array value
            // (Empty after a plain Dim, a number, ...) is replaced by the array.
            Variant* cell = f.scope ? f.scope->find(d.name) : global.find(d.name);
            if (!cell) cell = &(f.scope ? *f.scope : global)[d.name];
            // a fixed array is error 10; plain ReDim reports it before evaluating
            // the bounds, ReDim Preserve after (so a bad bound there is 13), as
            // Wine's conformance tests record for Windows
            bool fixedArr = cell->t == VT::Array && cell->arr && cell->arr->fixed;
            if (fixedArr && !s.b) throwErr(10, "This array is fixed or temporarily locked");
            std::vector<long long> dims;
            for (auto& de : d.dims) {
                long long n = toInt64(evalExpr(*de, f));
                if (n < -1) throwErr(5, errDesc(5)); // ReDim x(-1) is an empty array
                dims.push_back(n + 1);
            }
            if (cell->t != VT::Array || !cell->arr) {
                auto fresh = std::make_shared<VArray>();
                fresh->allocated = false;
                fresh->alloc(dims, optionBase, false);
                *cell = Variant::mkArr(fresh);
                continue;
            }
            VArray* arr = cell->arr.get();
            if (fixedArr) throwErr(10, "This array is fixed or temporarily locked");
            // ReDim Preserve may change only the upper bound of the last
            // dimension; a different dimension count or any other bound is
            // "Subscript out of range", as in VBScript.
            if (s.b && arr->allocated) {
                if (dims.size() != arr->dims.size()) throwErr(9, errDesc(9));
                for (size_t i = 0; i + 1 < dims.size(); i++)
                    if (dims[i] != arr->dims[i]) throwErr(9, errDesc(9));
            }
            arr->alloc(dims, optionBase, s.b);
        }
        break;
    }

    case Stmt::ConstDecl: {
        Scope& sc = f.scope ? *f.scope : global;
        for (size_t i = 0; i < s.decls.size(); i++)
            sc.vars[s.decls[i].name] = evalExpr(*s.exprs[i], f);
        break;
    }

    case Stmt::SubDecl:
    case Stmt::FuncDecl:
    case Stmt::PropDecl:
        registerProc(s, "");
        break;

    case Stmt::ClassDecl:
        registerClass(s);
        break;

    case Stmt::OnError: {
        if (s.b) { f.resumeNext = true; f.hasErrLabel = false; }
        else if (s.num == 0) { f.resumeNext = false; f.hasErrLabel = false; }
        else { f.resumeNext = false; f.hasErrLabel = true; f.errLabel = s.name; }
        break;
    }

    case Stmt::Exit:
        throw ExitSig{s.name};

    case Stmt::EndStmt:
        throw EndSig{};

    case Stmt::Goto:
        throw GotoSig{s.name};

    case Stmt::Option: {
        if (s.name == "explicit") optionExplicit = s.b;
        else if (s.name == "base") optionBase = (int)s.num;
        else if (s.name == "compare") compareText = s.b;
        break;
    }

    case Stmt::Randomize: {
        if (s.e) seedRng(toDouble(evalExpr(*s.e, f)));
        else seedRng((double)std::chrono::high_resolution_clock::now().time_since_epoch().count());
        break;
    }

    case Stmt::Erase: {
        for (auto& ex : s.exprs) {
            Variant* cell = findVar(f, ex->name);
            if (!cell) break;
            if (cell->t == VT::Array && cell->arr) {
                if (cell->arr->fixed) {
                    for (auto& v : cell->arr->data) v = Variant();
                } else {
                    cell->arr->allocated = false;
                    cell->arr->data.clear();
                    cell->arr->dims.clear();
                }
            } else {
                *cell = Variant();
            }
        }
        break;
    }

    case Stmt::Execute: {
        std::string code = toStr(evalExpr(*s.e, f));
        executeString(code, false);
        break;
    }
    case Stmt::ExecuteGlobal: {
        std::string code = toStr(evalExpr(*s.e, f));
        executeString(code, true);
        break;
    }
    }
}

// ---------- member base (supports the With ".member" shorthand) ----------

Variant Interp::evalMemberBase(Expr& e, Frame& f) {
    if (e.k == Expr::Var && e.name == "\x01" "with") {
        if (withStack.empty() || withStack.back().t != VT::Object || !withStack.back().obj)
            throwErr(424, errDesc(424));
        return withStack.back();
    }
    return evalExpr(e, f);
}

// ---------- assignment ----------

void Interp::assignTarget(Expr& t, Variant v, bool isSet, Frame& f) {
    if (isSet && v.t != VT::Object) throwErr(424, errDesc(424));
    switch (t.k) {
    case Expr::Var: {
        if (ClassInstance* me = classMemberFor(f, t.name, true)) {
            std::vector<Variant> noargs;
            me->letProp(t.name, noargs, v, isSet);
            return;
        }
        Variant* cell = findVar(f, t.name);
        if (!cell) {
            // With member?
            for (auto it = withStack.rbegin(); it != withStack.rend(); ++it) {
                if (it->t == VT::Object && !it->obj) break; // failed-With barrier
                if (it->t == VT::Object && it->obj && it->obj->hasMember(t.name)) {
                    it->obj->setMember(t.name, v);
                    return;
                }
            }
            // assigning to a procedure name (e.g. its return value slot at
            // global scope) is silently ignored, matching vbscript
            if (procs.count(t.name)) return;
            if (optionExplicit) throwErr(500, errDesc(500));
            cell = &f.scope->vars[t.name];
        }
        // obj = value on an object with a default setter dispatches to it
        // instead of replacing the object reference
        if (!isSet && cell->t == VT::Object && cell->obj && cell->obj->hasDefault() && v.t != VT::Object) {
            cell->obj->setMember("", v);
            return;
        }
        ownArrays(v); // b = a copies the array
        *cell = std::move(v);
        return;
    }
    // (Static vars are found via findVar, which checks f.proc->statics)
    case Expr::Member: {
        Variant base = evalMemberBase(*t.a[0], f);
        if (base.t != VT::Object || !base.obj) throwErr(424, errDesc(424));
        if (auto* ci = dynamic_cast<ClassInstance*>(base.obj.get())) {
            std::vector<Variant> noargs;
            ci->letProp(t.name, noargs, v, isSet);
            return;
        }
        base.obj->setMember(t.name, v);
        return;
    }
    case Expr::Call: {
        Expr& callee = *t.a[0];
        // obj.Prop(args) = v / Me.Prop(args) = v / Prop(args) = v inside the class
        ClassInstance* target = nullptr;
        std::string member;
        Variant holder; // keeps the object alive while its property runs
        if (callee.k == Expr::Member) {
            holder = evalMemberBase(*callee.a[0], f);
            if (holder.t != VT::Object || !holder.obj) throwErr(424, errDesc(424));
            target = dynamic_cast<ClassInstance*>(holder.obj.get());
            member = callee.name;
        } else if (callee.k == Expr::Var) {
            target = classMemberFor(f, callee.name, true);
            if (!target) target = classMemberFor(f, callee.name, false);
            member = callee.name;
        }
        if (target) {
            std::vector<Variant> args;
            for (size_t i = 1; i < t.a.size(); i++) args.push_back(evalExpr(*t.a[i], f));
            target->letProp(member, args, v, isSet);
            return;
        }
        Variant base = callee.k == Expr::Member ? holder.obj->getMember(member) : evalExpr(callee, f);
        std::vector<Variant> args;
        for (size_t i = 1; i < t.a.size(); i++) args.push_back(evalExpr(*t.a[i], f));
        if (base.t == VT::Array) {
            std::vector<long long> idx;
            for (auto& av : args) idx.push_back(toInt64(av));
            ownArrays(v);
            base.arr->at(idx) = std::move(v);
            return;
        }
        if (base.t == VT::Object && base.obj) {
            if (auto* ci = dynamic_cast<ClassInstance*>(base.obj.get())) {
                ci->letProp("", args, v, isSet); // default property: obj(args) = v
                return;
            }
            base.obj->assignInvoke(args, v);
            return;
        }
        throwErr(424, errDesc(424));
    }
    default:
        throwErr(501, errDesc(501));
    }
}

// ---------- expressions ----------

ArgVal Interp::evalArg(Expr& e, Frame& f) {
    ArgVal av;
    av.ref = evalLValue(e, f);
    av.val = evalExpr(e, f);
    if (av.ref) av.val = *av.ref;
    return av;
}

Variant* Interp::evalLValue(Expr& e, Frame& f) {
    if (e.paren) return nullptr; // (x) passes a copy even to a ByRef parameter
    if (e.k == Expr::Var) {
        // with-members are not stable lvalues; skip them
        if (Variant* v = f.scope->find(e.name)) return v;
        return nullptr;
    }
    if (e.k == Expr::Call) {
        // array element?
        Expr& base = *e.a[0];
        if (base.k == Expr::Var) {
            if (Variant* cell = f.scope->find(base.name)) {
                if (cell->t == VT::Array && cell->arr) {
                    std::vector<long long> idx;
                    for (size_t i = 1; i < e.a.size(); i++) idx.push_back(toInt64(evalExpr(*e.a[i], f)));
                    return &cell->arr->at(idx);
                }
            }
        }
        return nullptr;
    }
    return nullptr;
}

Variant Interp::evalCall(Expr& call, Frame& f) {
    Expr& callee = *call.a[0];
    std::vector<ArgVal> args;
    for (size_t i = 1; i < call.a.size(); i++) args.push_back(evalArg(*call.a[i], f));
    std::vector<Variant> vals;
    for (auto& a : args) vals.push_back(a.val);

    if (callee.k == Expr::Var) {
        const std::string& name = callee.name;
        if (ClassInstance* me = classMemberFor(f, name, false)) {
            std::shared_ptr<ClassInstance> keep = f.self;
            return me->getProp(name, args);
        }
        if (Variant* cell = f.scope->find(name)) {
            if (cell->t == VT::Array && cell->arr) {
                std::vector<long long> idx;
                for (auto& v : vals) idx.push_back(toInt64(v));
                return cell->arr->at(idx);
            }
            if (cell->t == VT::Object) {
                if (!cell->obj) throwErr(424, errDesc(424));
                return cell->obj->invoke(vals);
            }
            // a scalar variable shadows nothing callable; fall through to procs/builtins
            // (e.g. the function return-value slot inside a recursive call)
            if (f.self && f.self->hasProc(name, false) && f.scope->vars.count(name))
                return f.self->getProp(name, args); // recursion inside a class member
        }
        // inside a class method, an unqualified name resolves to a class member
        if (f.self && f.self->cls) {
            if (Proc* p = f.self->findProc(name, 0)) return callProc(*p, args, f.self);
        }
        if (auto it = procs.find(name); it != procs.end()) return callProc(*it->second, args, f.self);
        if (auto it = builtins.find(name); it != builtins.end()) {
            if ((int)vals.size() < it->second.minArgs || (int)vals.size() > it->second.maxArgs)
                throwErr(450, errDesc(450));
            return it->second.fn(*this, vals);
        }
        if (f.scope->find(name)) throwErr(424, errDesc(424));
        if (optionExplicit) throwErr(500, errDesc(500));
        throwErr(35, errDesc(35));
    }
    if (callee.k == Expr::Member) {
        Variant base = evalMemberBase(*callee.a[0], f);
        if (base.t != VT::Object || !base.obj) throwErr(424, errDesc(424));
        if (auto* ci = dynamic_cast<ClassInstance*>(base.obj.get()))
            return ci->getProp(callee.name, args); // keeps ByRef arguments
        if (base.obj->hasMethod(callee.name)) return base.obj->callMember(callee.name, vals);
        // property that is itself callable/indexable (e.g. WScript.Arguments(i), dict.Item(k))
        Variant v = base.obj->getMember(callee.name);
        if (v.t == VT::Array && v.arr) {
            std::vector<long long> idx;
            for (auto& x : vals) idx.push_back(toInt64(x));
            return v.arr->at(idx);
        }
        if (v.t == VT::Object && v.obj) return v.obj->invoke(vals);
        if (vals.empty()) return v;
        // scalar member invoked with args: a class Property Get with parameters
        return base.obj->callMember(callee.name, vals);
    }
    // generic: arr(i) or obj(args)
    Variant base = evalExpr(callee, f);
    if (base.t == VT::Array && base.arr) {
        std::vector<long long> idx;
        for (auto& v : vals) idx.push_back(toInt64(v));
        return base.arr->at(idx);
    }
    if (base.t == VT::Object && base.obj) return base.obj->invoke(vals);
    throwErr(424, errDesc(424));
}

Variant Interp::evalExpr(Expr& e, Frame& f) {
    switch (e.k) {
    case Expr::Lit:
        return e.lit;
    case Expr::Var: {
        if (ClassInstance* me = classMemberFor(f, e.name, false)) {
            std::vector<ArgVal> noargs;
            return me->getProp(e.name, noargs);
        }
        if (Variant* cell = findVar(f, e.name)) return *cell;
        // Me at global scope is the global object itself
        if (e.name == "me" && !f.self && globalThis) return Variant::mkObj(globalThis);
        // constants (vbCrLf, vbOK, ...)
        if (auto it = constants.find(e.name); it != constants.end()) return it->second;
        // parameterless builtins/procs used without parens: Now, Date, Rnd, ...
        if (auto it = builtins.find(e.name); it != builtins.end() && it->second.minArgs == 0) {
            std::vector<Variant> noargs;
            return it->second.fn(*this, noargs);
        }
        if (auto it = procs.find(e.name); it != procs.end()) {
            bool allOptional = true;
            for (auto& pm : *it->second->params) if (!pm.optional && !pm.hasDefault) { allOptional = false; break; }
            if (allOptional) {
                std::vector<ArgVal> noargs;
                return callProc(*it->second, noargs, f.self);
            }
        }
        if (optionExplicit) throwErr(500, errDesc(500));
        return Variant();
    }
    case Expr::Unary: {
        Variant v = evalExpr(*e.a[0], f);
        if (e.op == "-") return opNeg(v);
        if (e.op == "not") return opNot(v);
        throwErr(5, errDesc(5));
    }
    case Expr::Bin: {
        Variant l = evalExpr(*e.a[0], f);
        Variant r = evalExpr(*e.a[1], f);
        const std::string& op = e.op;
        if (op == "+") return opAdd(l, r);
        if (op == "-") return opSub(l, r);
        if (op == "*") return opMul(l, r);
        if (op == "/") return opDiv(l, r);
        if (op == "\\") return opIDiv(l, r);
        if (op == "mod") return opMod(l, r);
        if (op == "^") return opPow(l, r);
        if (op == "&") return opConcat(l, r);
        if (op == "and" || op == "or" || op == "xor" || op == "eqv" || op == "imp")
            return opLogic(op, l, r);
        if (op == "is") return Variant::mkBool(objSame(l, r));
        return opCompare(op, l, r, compareText);
    }
    case Expr::Member: {
        Variant base = evalMemberBase(*e.a[0], f);
        if (base.t == VT::Object && base.obj) return base.obj->getMember(e.name);
        throwErr(424, errDesc(424));
    }
    case Expr::Call:
        return evalCall(e, f);
    case Expr::NewExpr: {
        std::vector<Expr*> argExprs;
        for (auto& a : e.a) argExprs.push_back(a.get());
        return instantiateClass(e.name, argExprs, f, e.line);
    }
    }
    throwErr(5, errDesc(5));
}

// ---------- procedure calls ----------

Variant Interp::callProc(Proc& p, std::vector<ArgVal>& args, std::shared_ptr<ClassInstance> self) {
    Variant r = callProcBody(p, args, std::move(self));
    // the frame is gone: run Class_Terminate for locals that held the last reference
    runPendingTerminators();
    return r;
}

Variant Interp::callProcBody(Proc& p, std::vector<ArgVal>& args, std::shared_ptr<ClassInstance> self) {
    // recursion guard: raise VBScript error 28 instead of overflowing the C++ stack
    constexpr int MAX_CALL_DEPTH = 1000;
    if (++callDepth > MAX_CALL_DEPTH) {
        callDepth--;
        throwErr(28, errDesc(28));
    }
    struct DepthGuard { Interp& i; ~DepthGuard() { i.callDepth--; } } guard { *this };
    Frame f;
    f.local = std::make_unique<Scope>();
    f.local->parent = self ? &self->fields : &global; // class methods see instance fields
    f.scope = f.local.get();
    f.proc = &p;
    f.self = self;
    if (self) (*f.scope)["me"] = Variant::mkObj(self);
    if (!p.isSub) (*f.scope)[p.name] = Variant(); // return-value slot

    std::vector<std::pair<std::string, Variant*>> byRef;
    const std::vector<Param>& params = *p.params;
    for (size_t i = 0; i < params.size(); i++) {
        const Param& pm = params[i];
        Variant v;
        if (i < args.size()) v = args[i].val;
        else if (pm.hasDefault) v = evalExpr(*pm.def, f);
        else v = Variant();
        // ByRef with an lvalue argument shares the caller's array (writes to its
        // elements, ReDim and Erase reach the caller); everything else, ByVal or
        // a ByRef argument that is an expression such as (arr), gets a copy.
        bool aliased = pm.byRef && i < args.size() && args[i].ref;
        if (!aliased) ownArrays(v);
        (*f.scope)[pm.name] = std::move(v);
        if (aliased) byRef.push_back({pm.name, args[i].ref});
    }

    f.bodyList = &p.body;
    buildLabels(p.body, f.labelMap);

    Frame* saved = cur;
    cur = &f;
    // Restore cur on every exit path (normal, ExitSig, VBSError, GotoSig, ...):
    // with On Error Resume Next the caller keeps running after we throw, and a
    // dangling cur would crash later Eval/Execute calls.
    struct CurGuard { Interp& i; Frame* s; ~CurGuard() { i.cur = s; } } curGuard { *this, saved };
    try {
        execStmtList(p.body, f);
    } catch (ExitSig& x) {
        if (!((p.isSub && x.kind == "sub") || (!p.isSub && p.propKind == 0 && x.kind == "function") ||
              (p.propKind != 0 && x.kind == "property")))
            throw;
    }

    // ByRef copy-back
    for (auto& pr : byRef) {
        auto it = f.scope->vars.find(pr.first);
        if (it != f.scope->vars.end()) *pr.second = it->second;
    }

    if (!p.isSub) {
        auto it = f.scope->vars.find(p.name);
        if (it != f.scope->vars.end()) return it->second;
    }
    return Variant();
}

Variant Interp::callByName(const std::string& lowerName, std::vector<ArgVal>& args) {
    if (auto it = procs.find(lowerName); it != procs.end()) return callProc(*it->second, args, nullptr);
    if (auto it = builtins.find(lowerName); it != builtins.end()) {
        std::vector<Variant> vals;
        for (auto& a : args) vals.push_back(a.val);
        return it->second.fn(*this, vals);
    }
    throwErr(35, errDesc(35));
}

// ---------- classes ----------

Variant Interp::instantiateClass(const std::string& name, std::vector<Expr*>& argExprs, Frame& f, int line) {
    auto it = classes.find(name);
    if (it == classes.end()) {
        // not a script-defined class: try a host ProgID (e.g. New RegExp)
        auto host = createHostObject(*this, name);
        if (host) return Variant::mkObj(host);
        VBSError e(430, "Class not defined: " + name);
        e.line = line;
        throw e;
    }
    ClassInfo& ci = it->second;
    auto inst = ClassInstance::create(*this, ci);
    // initialize fields
    for (auto* dp : ci.fields) {
        Decl& d = *dp;
        if (!d.isArray) inst->fields.vars[d.name] = Variant();
        else {
            auto arr = std::make_shared<VArray>();
            arr->base = optionBase;
            if (d.dynamic) arr->allocated = false;
            else {
                std::vector<long long> dims;
                for (auto& de : d.dims) {
                    long long n = toInt64(evalExpr(*de, f));
                    if (n < 0) throwErr(5, errDesc(5));
                    dims.push_back(n + 1);
                }
                arr->alloc(dims, optionBase, false);
                arr->fixed = true;
            }
            inst->fields.vars[d.name] = Variant::mkArr(arr);
        }
    }
    // call Class_Initialize if present (or a parameterless Initialize)
    for (Proc* p : ci.procs) {
        if (p->propKind == 0 && (p->name == "class_initialize" || (p->name == "initialize" && p->params->empty()))) {
            std::vector<ArgVal> av;
            for (auto* ae : argExprs) { ArgVal x; x.val = evalExpr(*ae, f); av.push_back(x); }
            callProc(*p, av, inst);
            break;
        }
    }
    return Variant::mkObj(inst);
}

// ---------- Eval / Execute ----------

Variant Interp::evalString(const std::string& code) {
    ExprPtr e = Parser::parseExpressionString(code);
    Frame& f = *cur;
    return evalExpr(*e, f);
}

void Interp::executeString(const std::string& code, bool globalScope) {
    auto prog = std::make_unique<Program>(Parser::parseStatementsString(code));
    std::vector<Stmt*> list;
    for (auto& st : prog->stmts) list.push_back(st.get());
    ownedPrograms.push_back(std::move(prog));

    Frame f;
    f.scope = globalScope ? &global : (cur ? cur->scope : &global);
    f.bodyList = &list;
    buildLabels(list, f.labelMap);
    if (cur) f.resumeNext = cur->resumeNext;
    Frame* saved = cur;
    cur = &f;
    struct CurGuard { Interp& i; Frame* s; ~CurGuard() { i.cur = s; } } curGuard { *this, saved };
    try {
        execStmtList(list, f);
    } catch (EndSig&) {
    }
}
