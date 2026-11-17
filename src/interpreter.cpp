#include "interpreter.h"

#include "builtins.h"
#include "objects.h"
#include "parser.h"

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

bool ClassInstance::hasMethod(const std::string& n) const {
    if (!cls) return false;
    for (Proc* p : cls->procs) if (p->name == n && p->propKind == 0) return true;
    return false;
}

Variant ClassInstance::getMember(const std::string& n) {
    auto it = fields.vars.find(n);
    if (it != fields.vars.end()) return it->second;
    if (Proc* p = findProc(n, 1)) { // Property Get
        std::vector<ArgVal> noargs;
        return interp->callProc(*p, noargs, self);
    }
    throwErr(438, errDesc(438));
}

void ClassInstance::setMember(const std::string& n, const Variant& v) {
    auto it = fields.vars.find(n);
    if (it != fields.vars.end()) { it->second = v; return; }
    if (Proc* p = findProc(n, 2)) { // Property Let
        std::vector<ArgVal> args(p->params->size());
        for (size_t i = 0; i + 1 < p->params->size(); i++) args[i].val = Variant();
        args.back().val = v;
        interp->callProc(*p, args, self);
        return;
    }
    if (Proc* p = findProc(n, 3)) { // Property Set
        std::vector<ArgVal> args(p->params->size());
        for (size_t i = 0; i + 1 < p->params->size(); i++) args[i].val = Variant();
        args.back().val = v;
        interp->callProc(*p, args, self);
        return;
    }
    throwErr(438, errDesc(438));
}

Variant ClassInstance::callMember(const std::string& n, std::vector<Variant>& args) {
    if (!cls) throwErr(438, errDesc(438));
    for (Proc* p : cls->procs) {
        if (p->name == n && p->propKind == 0) {
            std::vector<ArgVal> av;
            for (auto& a : args) { ArgVal x; x.val = a; av.push_back(x); }
            return interp->callProc(*p, av, self);
        }
    }
    // property get used as call? no.
    throwErr(438, errDesc(438));
}

// ---------- Interp ----------

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
            ci.procs.push_back(registerProc(*m, s.name));
        }
    }
    classes[s.name] = std::move(ci);
}

void Interp::load(Program& p, const std::string& srcName) {
    prog = &p;
    sourceName = srcName;
    for (auto& st : p.stmts) {
        topLevel.push_back(st.get());
        if (st->k == Stmt::SubDecl || st->k == Stmt::FuncDecl || st->k == Stmt::PropDecl)
            registerProc(*st, "");
        else if (st->k == Stmt::ClassDecl)
            registerClass(*st);
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
    // With stack (innermost first)
    for (auto it = withStack.rbegin(); it != withStack.rend(); ++it) {
        if (it->t == VT::Object && it->obj && it->obj->hasMember(name)) {
            auto tmp = std::make_unique<Variant>(it->obj->getMember(name));
            Variant* raw = tmp.get();
            f.scratch.push_back(std::move(tmp));
            return raw;
        }
    }
    return nullptr;
}

// ---------- statements ----------

static void buildLabels(const std::vector<Stmt*>& list, std::map<std::string, size_t>& m) {
    for (size_t i = 0; i < list.size(); i++)
        if (list[i]->k == Stmt::Label) m[list[i]->name] = i;
}

void Interp::execStmtList(const std::vector<Stmt*>& list, Frame& f) {
    size_t i = 0;
    while (i < list.size()) {
        Stmt& s = *list[i];
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
        assignTarget(*s.e, v, s.b, f);
        break;
    }

    case Stmt::If: {
        if (toBool(evalExpr(*s.e, f))) {
            execStmtList(ptrVec(s.body), f);
            break;
        }
        for (auto& arm : s.arms) {
            if (toBool(evalExpr(*arm.first[0], f))) {
                execStmtList(ptrVec(arm.second), f);
                return;
            }
        }
        if (!s.body2.empty()) execStmtList(ptrVec(s.body2), f);
        break;
    }

    case Stmt::Select: {
        Variant cv = evalExpr(*s.e, f);
        for (auto& arm : s.arms) {
            if (arm.first.empty()) { // Case Else
                std::vector<Stmt*> v; for (auto& x : arm.second) v.push_back(x.get());
                execStmtList(v, f);
                return;
            }
            bool matched = false;
            for (auto& item : arm.first) {
                bool m;
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
        double start = toDouble(evalExpr(*s.e2, f));
        double end = toDouble(evalExpr(*s.exprs[0], f));
        double step = s.e3 ? toDouble(evalExpr(*s.e3, f)) : 1.0;
        if (step == 0) throwErr(5, errDesc(5));
        *cell = numToVar(start);
        auto body = ptrVec(s.body);
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
        Variant coll = evalExpr(*s.e2, f);
        std::vector<Variant> items;
        if (coll.t == VT::Array) {
            if (!coll.arr || !coll.arr->allocated) throwErr(9, errDesc(9));
            items.assign(coll.arr->data.begin(), coll.arr->data.end());
        } else if (coll.t == VT::Object) {
            if (!coll.obj) throwErr(424, errDesc(424));
            items = coll.obj->enumItems();
        } else {
            throwErr(424, errDesc(424));
        }
        auto body = ptrVec(s.body);
        for (auto& item : items) {
            *cell = item;
            try {
                execStmtList(body, f);
            } catch (ExitSig& x) {
                if (x.kind == "for") break;
                throw;
            }
        }
        break;
    }

    case Stmt::DoLoop: {
        auto body = ptrVec(s.body);
        for (;;) {
            if (s.name == "while") { if (!toBool(evalExpr(*s.e, f))) break; }
            else if (s.name == "until") { if (toBool(evalExpr(*s.e, f))) break; }
            try {
                execStmtList(body, f);
            } catch (ExitSig& x) {
                if (x.kind == "do") break;
                throw;
            }
            if (s.name2 == "while") { if (!toBool(evalExpr(*s.e2, f))) break; }
            else if (s.name2 == "until") { if (toBool(evalExpr(*s.e2, f))) break; }
            // no conditions at all: Do ... Loop is an infinite loop in VBScript
        }
        break;
    }

    case Stmt::While: {
        auto body = ptrVec(s.body);
        while (toBool(evalExpr(*s.e, f))) execStmtList(body, f);
        break;
    }

    case Stmt::With: {
        Variant v = evalExpr(*s.e, f);
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
            Variant* cell = f.scope ? f.scope->find(d.name) : global.find(d.name);
            if (!cell || cell->t != VT::Array || !cell->arr) throwErr(13, errDesc(13));
            VArray* arr = cell->arr.get();
            if (arr->fixed) throwErr(10, "This array is fixed or temporarily locked");
            std::vector<long long> dims;
            for (auto& de : d.dims) {
                long long n = toInt64(evalExpr(*de, f));
                if (n < 0) throwErr(5, errDesc(5));
                dims.push_back(n + 1);
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

void Interp::assignTarget(Expr& t, const Variant& v, bool isSet, Frame& f) {
    if (isSet && v.t != VT::Object) throwErr(424, errDesc(424));
    switch (t.k) {
    case Expr::Var: {
        Variant* cell = findVar(f, t.name);
        if (!cell) {
            // With member?
            for (auto it = withStack.rbegin(); it != withStack.rend(); ++it) {
                if (it->t == VT::Object && it->obj && it->obj->hasMember(t.name)) {
                    it->obj->setMember(t.name, v);
                    return;
                }
            }
            if (optionExplicit) throwErr(500, errDesc(500));
            cell = &f.scope->vars[t.name];
        }
        *cell = v;
        return;
    }
    // (Static vars are found via findVar, which checks f.proc->statics)
    case Expr::Member: {
        Variant base = evalMemberBase(*t.a[0], f);
        if (base.t != VT::Object || !base.obj) throwErr(424, errDesc(424));
        base.obj->setMember(t.name, v);
        return;
    }
    case Expr::Call: {
        Variant base = evalExpr(*t.a[0], f);
        std::vector<Variant> args;
        for (size_t i = 1; i < t.a.size(); i++) args.push_back(evalExpr(*t.a[i], f));
        if (base.t == VT::Array) {
            std::vector<long long> idx;
            for (auto& av : args) idx.push_back(toInt64(av));
            base.arr->at(idx) = v;
            return;
        }
        if (base.t == VT::Object && base.obj) {
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
        throwErr(438, errDesc(438));
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
        if (Variant* cell = findVar(f, e.name)) return *cell;
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
        (*f.scope)[pm.name] = v;
        if (pm.byRef && i < args.size() && args[i].ref)
            byRef.push_back({pm.name, args[i].ref});
    }

    f.bodyList = &p.body;
    buildLabels(p.body, f.labelMap);

    Frame* saved = cur;
    cur = &f;
    try {
        execStmtList(p.body, f);
    } catch (ExitSig& x) {
        if ((p.isSub && x.kind == "sub") || (!p.isSub && p.propKind == 0 && x.kind == "function") ||
            (p.propKind != 0 && x.kind == "property")) {
            // normal exit
        } else {
            cur = saved;
            throw;
        }
    }
    cur = saved;

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
        VBSError e(430, "Class not defined: " + name);
        e.line = line;
        throw e;
    }
    ClassInfo& ci = it->second;
    auto inst = std::make_shared<ClassInstance>();
    inst->interp = this;
    inst->cls = &ci;
    inst->fields.parent = &global;
    inst->self = inst;
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
    try {
        execStmtList(list, f);
    } catch (EndSig&) {
    }
    cur = saved;
}
