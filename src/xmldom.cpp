// xmldom.cpp — MSXML2.DOMDocument-compatible DOM, parsed with bsdxml (expat 2).
//
// Supported surface (the subset commonly used from VBScript):
//   document: async, documentElement, parseError, xml, load, loadXML, save,
//             createElement, createTextNode, createComment, createAttribute,
//             getElementsByTagName, selectNodes, selectSingleNode
//   node:     nodeName, nodeValue, text, nodeType, xml, parentNode, firstChild,
//             lastChild, nextSibling, previousSibling, childNodes, attributes,
//             getAttribute, setAttribute, removeAttribute, appendChild,
//             removeChild, replaceChild, insertBefore, cloneNode, hasChildNodes,
//             selectNodes, selectSingleNode, getElementsByTagName
//   lists:    length, item(i), numeric indexing, For Each
//   XPath subset: /a/b, //tag, *, ., .., text(), @attr, [n], [@a], [@a='v'],
//                 [tag], [tag='v'], [contains(@a,'v')]
#include "xmldom.h"
#include "interpreter.h"

#include <bsdxml.h>

#include <algorithm>
#include <cctype>
#include <climits>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>

namespace fs = std::filesystem;

// ---------- node model ----------

enum XmlNodeType { XML_ELEMENT = 1, XML_ATTRIBUTE = 2, XML_TEXT = 3, XML_CDATA = 4, XML_COMMENT = 8, XML_DOCUMENT = 9 };

struct XmlNode;
using XmlNodePtr = std::shared_ptr<XmlNode>;

struct XmlNode : VBObject {
    int type = XML_ELEMENT;
    std::string name;              // element/attribute name; "#text", "#cdata-section", "#comment", "#document"
    std::string value;             // text/attr/comment content
    std::vector<XmlNodePtr> children;
    std::vector<XmlNodePtr> attrs; // element attributes (in order)
    std::weak_ptr<XmlNode> parent;
    std::weak_ptr<XmlNode> self;

    static XmlNodePtr make(int t, const std::string& n, const std::string& v = "") {
        auto p = std::make_shared<XmlNode>();
        p->type = t; p->name = n; p->value = v;
        p->self = p;
        return p;
    }
    XmlNodePtr parentPtr() const { return parent.lock(); }

    std::string textContent() const {
        if (type == XML_TEXT || type == XML_CDATA || type == XML_COMMENT) return value;
        std::string out;
        for (auto& c : children) out += c->textContent();
        return out;
    }
    XmlNodePtr childAt(size_t i) const { return i < children.size() ? children[i] : nullptr; }
    XmlNodePtr sibling(bool next) const {
        auto p = parentPtr();
        if (!p) return nullptr;
        for (size_t i = 0; i < p->children.size(); i++) {
            if (p->children[i].get() == this) {
                size_t j = next ? i + 1 : (i == 0 ? 0 : i - 1);
                if (next && j < p->children.size()) return p->children[j];
                if (!next && i > 0) return p->children[i - 1];
                return nullptr;
            }
        }
        return nullptr;
    }
    XmlNodePtr findAttr(const std::string& n) const {
        for (auto& a : attrs) if (a->name == n) return a;
        return nullptr;
    }

    // ---- serialization ----
    static std::string escText(const std::string& s) {
        std::string o;
        for (char c : s) {
            switch (c) {
                case '&': o += "&amp;"; break;
                case '<': o += "&lt;"; break;
                case '>': o += "&gt;"; break;
                default: o += c;
            }
        }
        return o;
    }
    static std::string escAttr(const std::string& s) {
        std::string o;
        for (char c : s) {
            switch (c) {
                case '&': o += "&amp;"; break;
                case '<': o += "&lt;"; break;
                case '"': o += "&quot;"; break;
                default: o += c;
            }
        }
        return o;
    }
    std::string serialize() const {
        switch (type) {
            case XML_DOCUMENT: {
                std::string o = "<?xml version=\"1.0\"?>";
                for (auto& c : children) o += c->serialize();
                return o;
            }
            case XML_ELEMENT: {
                std::string o = "<" + name;
                for (auto& a : attrs) o += " " + a->name + "=\"" + escAttr(a->value) + "\"";
                if (children.empty()) return o + "/>";
                o += ">";
                for (auto& c : children) o += c->serialize();
                return o + "</" + name + ">";
            }
            case XML_TEXT: return escText(value);
            case XML_CDATA: return "<![CDATA[" + value + "]]>";
            case XML_COMMENT: return "<!--" + value + "-->";
            case XML_ATTRIBUTE: return name + "=\"" + escAttr(value) + "\"";
        }
        return "";
    }

    // ---- VBObject interface ----
    std::string objectKind() const override { return "IXMLDOMNode"; }

    bool hasMember(const std::string& n) const override {
        static const char* mems[] = {"nodename","nodevalue","text","nodetype","xml","parentnode",
            "firstchild","lastchild","nextsibling","previoussibling","childnodes","attributes",
            "basename","namespaceuri","prefix","localname","nodeTypeString"};
        for (auto m : mems) if (n == m) return true;
        return hasMethod(n);
    }
    bool hasMethod(const std::string& n) const override {
        static const char* ms[] = {"getattribute","setattribute","removeattribute","appendchild",
            "removechild","replacechild","insertbefore","clonenode","haschildnodes","selectnodes",
            "selectsinglenode","getelementsbytagname","getattributenode","transformnode"};
        for (auto m : ms) if (n == m) return true;
        return false;
    }

    Variant getMember(const std::string& n) override;
    void setMember(const std::string& n, const Variant& v) override;
    Variant callMember(const std::string& n, std::vector<Variant>& args) override;
};

// ---------- node lists ----------

struct XmlNodeList : VBObject {
    std::vector<XmlNodePtr> items;
    std::string kind = "IXMLDOMNodeList";
    bool hasMember(const std::string& n) const override { return n == "length" || n == "item"; }
    bool hasMethod(const std::string& n) const override { return n == "item" || n == "nextnode" || n == "reset"; }
    Variant getMember(const std::string& n) override {
        if (n == "length") return Variant::mkLong((long long)items.size());
        throwErr(438, errDesc(438));
    }
    Variant callMember(const std::string& n, std::vector<Variant>& args) override {
        if (n == "item") {
            long long i = args.empty() ? 0 : toInt64(args[0]);
            if (i < 0 || (size_t)i >= items.size()) return Variant::mkObj(nullptr); // Nothing
            return Variant::mkObj(items[(size_t)i]);
        }
        if (n == "nextnode") return Variant::mkObj(nullptr);
        if (n == "reset") return Variant();
        throwErr(438, errDesc(438));
    }
    Variant invoke(const std::vector<Variant>& args) override {
        long long i = args.empty() ? 0 : toInt64(args[0]);
        if (i < 0 || (size_t)i >= items.size()) throwErr(9, errDesc(9));
        return Variant::mkObj(items[(size_t)i]);
    }
    std::vector<Variant> enumItems() override {
        std::vector<Variant> out;
        for (auto& it : items) out.push_back(Variant::mkObj(it));
        return out;
    }
    std::string objectKind() const override { return kind; }
};

struct XmlAttrMap : VBObject {
    std::vector<XmlNodePtr>* attrs;
    explicit XmlAttrMap(std::vector<XmlNodePtr>* a) : attrs(a) {}
    bool hasMember(const std::string& n) const override { return n == "length" || n == "item" || n == "getnameditem"; }
    bool hasMethod(const std::string& n) const override { return n == "item" || n == "getnameditem" || n == "removenameditem" || n == "setnameditem"; }
    Variant getMember(const std::string& n) override {
        if (n == "length") return Variant::mkLong((long long)attrs->size());
        throwErr(438, errDesc(438));
    }
    Variant callMember(const std::string& n, std::vector<Variant>& args) override {
        if (n == "item") {
            long long i = args.empty() ? 0 : toInt64(args[0]);
            if (i < 0 || (size_t)i >= attrs->size()) return Variant::mkObj(nullptr);
            return Variant::mkObj((*attrs)[(size_t)i]);
        }
        if (n == "getnameditem") {
            std::string name = toStr(args[0]);
            for (auto& a : *attrs) if (a->name == name) return Variant::mkObj(a);
            return Variant::mkObj(nullptr);
        }
        if (n == "removenameditem") {
            std::string name = toStr(args[0]);
            for (size_t i = 0; i < attrs->size(); i++)
                if ((*attrs)[i]->name == name) { auto r = (*attrs)[i]; attrs->erase(attrs->begin() + i); return Variant::mkObj(r); }
            return Variant::mkObj(nullptr);
        }
        if (n == "setnameditem") {
            // arg is an attribute node
            const Variant& v = args[0];
            if (v.t == VT::Object && v.obj) {
                auto an = std::dynamic_pointer_cast<XmlNode>(v.obj);
                if (an) {
                    for (size_t i = 0; i < attrs->size(); i++)
                        if ((*attrs)[i]->name == an->name) { (*attrs)[i] = an; return Variant::mkObj(an); }
                    attrs->push_back(an);
                    return Variant::mkObj(an);
                }
            }
            throwErr(13, errDesc(13));
        }
        throwErr(438, errDesc(438));
    }
    Variant invoke(const std::vector<Variant>& args) override {
        long long i = args.empty() ? 0 : toInt64(args[0]);
        if (i < 0 || (size_t)i >= attrs->size()) throwErr(9, errDesc(9));
        return Variant::mkObj((*attrs)[(size_t)i]);
    }
    std::vector<Variant> enumItems() override {
        std::vector<Variant> out;
        for (auto& a : *attrs) out.push_back(Variant::mkObj(a));
        return out;
    }
    std::string objectKind() const override { return "IXMLDOMNamedNodeMap"; }
};

// ---------- XPath subset ----------

static void collectDescendants(const XmlNodePtr& n, std::vector<XmlNodePtr>& out) {
    for (auto& c : n->children) {
        if (c->type == XML_ELEMENT) {
            out.push_back(c);
            collectDescendants(c, out);
        }
    }
}

struct XPathStep {
    bool descendant = false; // //axis
    std::string name;         // "*", ".", "..", "text()", "@attr", tag
    // predicates
    bool hasNum = false; long long num = 0;
    bool hasAttrTest = false; std::string attrName;
    bool hasAttrEq = false; std::string attrEqName, attrEqVal;
    bool hasChildTest = false; std::string childName;
    bool hasChildEq = false; std::string childEqName, childEqVal;
    bool hasContains = false; std::string contAttr, contVal;
};

static std::vector<XPathStep> parseXPath(const std::string& xp) {
    std::vector<XPathStep> steps;
    size_t i = 0, n = xp.size();
    auto skipWs = [&]() { while (i < n && (xp[i] == ' ' || xp[i] == '\t')) i++; };
    skipWs();
    while (i < n) {
        XPathStep st;
        if (xp[i] == '/' && i + 1 < n && xp[i + 1] == '/') { st.descendant = true; i += 2; }
        else if (xp[i] == '/') { i++; }
        skipWs();
        // read step name until '/', '[', or end
        size_t start = i;
        while (i < n && xp[i] != '/' && xp[i] != '[') i++;
        st.name = xp.substr(start, i - start);
        // trim
        while (!st.name.empty() && (st.name.back() == ' ' || st.name.back() == '\t')) st.name.pop_back();
        size_t ns = 0;
        while (ns < st.name.size() && (st.name[ns] == ' ' || st.name[ns] == '\t')) ns++;
        st.name = st.name.substr(ns);
        // predicates
        while (i < n && xp[i] == '[') {
            i++;
            size_t ps = i;
            while (i < n && xp[i] != ']') i++;
            std::string pred = xp.substr(ps, i - ps);
            if (i < n) i++;
            // trim pred
            size_t a = 0, b = pred.size();
            while (a < b && std::isspace((unsigned char)pred[a])) a++;
            while (b > a && std::isspace((unsigned char)pred[b - 1])) b--;
            pred = pred.substr(a, b - a);
            // numeric
            bool allDigits = !pred.empty() && std::all_of(pred.begin(), pred.end(), [](char c){ return std::isdigit((unsigned char)c); });
            if (allDigits) { st.hasNum = true; st.num = std::atoll(pred.c_str()); }
            else if (pred.rfind("contains(", 0) == 0 && pred.back() == ')') {
                std::string inner = pred.substr(9, pred.size() - 10);
                auto comma = inner.find(',');
                if (comma != std::string::npos) {
                    std::string lhs = inner.substr(0, comma), rhs = inner.substr(comma + 1);
                    auto unq = [](std::string s) {
                        size_t a = 0, b = s.size();
                        while (a < b && std::isspace((unsigned char)s[a])) a++;
                        while (b > a && std::isspace((unsigned char)s[b - 1])) b--;
                        s = s.substr(a, b - a);
                        if (s.size() >= 2 && (s.front() == '\'' || s.front() == '"') && s.back() == s.front()) s = s.substr(1, s.size() - 2);
                        return s;
                    };
                    lhs = unq(lhs); rhs = unq(rhs);
                    if (lhs.rfind("@", 0) == 0) { st.hasContains = true; st.contAttr = lhs; st.contVal = rhs; }
                    else { st.hasContains = true; st.contAttr = (lhs == "text()" || lhs == ".") ? "\x02text" : lhs; st.contVal = rhs; }
                }
            }
            else if (pred.rfind("@", 0) == 0) {
                auto eq = pred.find('=');
                if (eq == std::string::npos) { st.hasAttrTest = true; st.attrName = pred.substr(1); }
                else {
                    st.hasAttrEq = true;
                    st.attrEqName = pred.substr(1, eq - 1);
                    std::string v = pred.substr(eq + 1);
                    if (v.size() >= 2 && (v.front() == '\'' || v.front() == '"') && v.back() == v.front()) v = v.substr(1, v.size() - 2);
                    st.attrEqVal = v;
                }
            }
            else {
                auto eq = pred.find('=');
                if (eq == std::string::npos) { st.hasChildTest = true; st.childName = pred; }
                else {
                    st.hasChildEq = true;
                    st.childEqName = pred.substr(0, eq);
                    std::string v = pred.substr(eq + 1);
                    if (v.size() >= 2 && (v.front() == '\'' || v.front() == '"') && v.back() == v.front()) v = v.substr(1, v.size() - 2);
                    st.childEqVal = v;
                }
            }
            skipWs();
        }
        steps.push_back(st);
        skipWs();
    }
    return steps;
}

static bool nameMatches(const XmlNodePtr& node, const std::string& pat) {
    if (pat == "*") return true;
    return node->name == pat;
}

static bool predMatches(const XmlNodePtr& node, const XPathStep& st) {
    if (st.hasAttrTest) return node->findAttr(st.attrName) != nullptr;
    if (st.hasAttrEq) {
        auto a = node->findAttr(st.attrEqName);
        return a && a->value == st.attrEqVal;
    }
    if (st.hasChildTest) {
        for (auto& c : node->children) if (c->type == XML_ELEMENT && c->name == st.childName) return true;
        return false;
    }
    if (st.hasChildEq) {
        for (auto& c : node->children)
            if (c->type == XML_ELEMENT && c->name == st.childEqName && c->textContent() == st.childEqVal) return true;
        return false;
    }
    if (st.hasContains) {
        std::string hay;
        if (st.contAttr == "\x02text") hay = node->textContent();
        else if (!st.contAttr.empty() && st.contAttr[0] == '@') {
            auto a = node->findAttr(st.contAttr.substr(1));
            hay = a ? a->value : "";
        } else {
            // child element text
            for (auto& c : node->children)
                if (c->type == XML_ELEMENT && c->name == st.contAttr) { hay = c->textContent(); break; }
        }
        return hay.find(st.contVal) != std::string::npos;
    }
    return true;
}

static std::vector<XmlNodePtr> xpathSelect(const XmlNodePtr& context, const std::string& xpathIn) {
    std::string xp = trim(xpathIn);
    std::vector<XmlNodePtr> current;
    size_t i = 0;
    if (!xp.empty() && xp[0] == '/') {
        // absolute: from the document node
        XmlNodePtr doc = context;
        while (auto p = doc->parentPtr()) doc = p;
        current.push_back(doc);
        i = 1;
        if (i < xp.size() && xp[i] == '/') i++;
        // rebuild steps from scratch: easier to re-parse
        auto steps = parseXPath(xp);
        // first step after leading / is applied to document children
        std::vector<XmlNodePtr> result = current;
        for (auto& st : steps) {
            std::vector<XmlNodePtr> next;
            for (auto& node : result) {
                std::vector<XmlNodePtr> cands;
                if (st.name == ".") { cands.push_back(node); }
                else if (st.name == "..") { if (auto p = node->parentPtr()) cands.push_back(p); }
                else if (st.name.rfind("@", 0) == 0) {
                    for (auto& a : node->attrs) if (nameMatches(a, st.name.substr(1))) cands.push_back(a);
                }
                else if (st.name == "text()") {
                    for (auto& c : node->children) if (c->type == XML_TEXT || c->type == XML_CDATA) cands.push_back(c);
                }
                else if (st.descendant) {
                    std::vector<XmlNodePtr> all;
                    collectDescendants(node, all);
                    for (auto& c : all) if (nameMatches(c, st.name)) cands.push_back(c);
                }
                else {
                    for (auto& c : node->children) if (c->type == XML_ELEMENT && nameMatches(c, st.name)) cands.push_back(c);
                }
                // apply predicates
                if (st.hasNum) {
                    if (st.num >= 1 && (size_t)st.num <= cands.size()) next.push_back(cands[(size_t)st.num - 1]);
                } else {
                    for (auto& c : cands) if (predMatches(c, st)) next.push_back(c);
                }
            }
            result = next;
        }
        return result;
    }
    // relative
    auto steps = parseXPath(xp);
    current.push_back(context);
    std::vector<XmlNodePtr> result = current;
    for (auto& st : steps) {
        std::vector<XmlNodePtr> next;
        for (auto& node : result) {
            std::vector<XmlNodePtr> cands;
            if (st.name == ".") cands.push_back(node);
            else if (st.name == "..") { if (auto p = node->parentPtr()) cands.push_back(p); }
            else if (st.name.rfind("@", 0) == 0) {
                for (auto& a : node->attrs) if (nameMatches(a, st.name.substr(1))) cands.push_back(a);
            }
            else if (st.name == "text()") {
                for (auto& c : node->children) if (c->type == XML_TEXT || c->type == XML_CDATA) cands.push_back(c);
            }
            else if (st.descendant) {
                std::vector<XmlNodePtr> all;
                collectDescendants(node, all);
                for (auto& c : all) if (nameMatches(c, st.name)) cands.push_back(c);
            }
            else {
                for (auto& c : node->children) if (c->type == XML_ELEMENT && nameMatches(c, st.name)) cands.push_back(c);
            }
            if (st.hasNum) {
                if (st.num >= 1 && (size_t)st.num <= cands.size()) next.push_back(cands[(size_t)st.num - 1]);
            } else {
                for (auto& c : cands) if (predMatches(c, st)) next.push_back(c);
            }
        }
        result = next;
    }
    return result;
}

static std::vector<XmlNodePtr> elementsByTagName(const XmlNodePtr& root, const std::string& name) {
    std::vector<XmlNodePtr> out;
    std::vector<XmlNodePtr> all;
    collectDescendants(root, all);
    if (root->type == XML_ELEMENT && nameMatches(root, name)) out.push_back(root);
    for (auto& c : all) if (nameMatches(c, name)) out.push_back(c);
    return out;
}

static std::shared_ptr<XmlNodeList> makeList(std::vector<XmlNodePtr> items, const std::string& kind = "IXMLDOMNodeList") {
    auto l = std::make_shared<XmlNodeList>();
    l->items = std::move(items);
    l->kind = kind;
    return l;
}

// ---------- XmlNode member access ----------

Variant XmlNode::getMember(const std::string& n) {
    auto selfPtr = self.lock();
    if (n == "nodename") return Variant::mkStr(name);
    if (n == "nodevalue") {
        if (type == XML_ELEMENT || type == XML_DOCUMENT) return Variant::mkNull();
        return Variant::mkStr(value);
    }
    if (n == "text") return Variant::mkStr(textContent());
    if (n == "nodetype") return Variant::mkInt(type);
    if (n == "xml") return Variant::mkStr(serialize());
    if (n == "parentnode") { auto p = parentPtr(); return p ? Variant::mkObj(p) : Variant::mkObj(nullptr); }
    if (n == "firstchild") return Variant::mkObj(childAt(0));
    if (n == "lastchild") return Variant::mkObj(childAt(children.empty() ? 0 : children.size() - 1));
    if (n == "nextsibling") return Variant::mkObj(sibling(true));
    if (n == "previoussibling") return Variant::mkObj(sibling(false));
    if (n == "childnodes") return Variant::mkObj(makeList(children));
    if (n == "attributes") {
        if (type != XML_ELEMENT) return Variant::mkObj(nullptr);
        return Variant::mkObj(std::make_shared<XmlAttrMap>(&attrs));
    }
    if (n == "basename" || n == "localname") {
        auto c = name.find(':');
        return Variant::mkStr(c == std::string::npos ? name : name.substr(c + 1));
    }
    if (n == "prefix") {
        auto c = name.find(':');
        return Variant::mkStr(c == std::string::npos ? "" : name.substr(0, c));
    }
    if (n == "namespaceuri") return Variant::mkStr("");
    if (n == "nodetypestring") {
        switch (type) {
            case XML_ELEMENT: return Variant::mkStr("element");
            case XML_ATTRIBUTE: return Variant::mkStr("attribute");
            case XML_TEXT: return Variant::mkStr("text");
            case XML_CDATA: return Variant::mkStr("cdatasection");
            case XML_COMMENT: return Variant::mkStr("comment");
            case XML_DOCUMENT: return Variant::mkStr("document");
        }
    }
    throwErr(438, errDesc(438));
}

void XmlNode::setMember(const std::string& n, const Variant& v) {
    if (n == "nodevalue") {
        if (type == XML_TEXT || type == XML_CDATA || type == XML_COMMENT || type == XML_ATTRIBUTE) {
            value = toStr(v);
            return;
        }
        throwErr(424, errDesc(424));
    }
    if (n == "text") {
        if (type == XML_ELEMENT || type == XML_DOCUMENT) {
            children.clear();
            auto t = XmlNode::make(XML_TEXT, "#text", toStr(v));
            t->parent = self;
            children.push_back(t);
            return;
        }
        value = toStr(v);
        return;
    }
    throwErr(438, errDesc(438));
}

static XmlNodePtr asNode(const Variant& v, int what) {
    if (v.t != VT::Object || !v.obj) throwErr(424, errDesc(424));
    auto n = std::dynamic_pointer_cast<XmlNode>(v.obj);
    if (!n) throwErr(13, errDesc(13));
    return n;
}

Variant XmlNode::callMember(const std::string& n, std::vector<Variant>& args) {
    auto selfPtr = self.lock();
    if (n == "getattribute") {
        std::string name = toStr(args[0]);
        auto a = findAttr(name);
        return Variant::mkStr(a ? a->value : "");
    }
    if (n == "getattributenode") {
        std::string name = toStr(args[0]);
        auto a = findAttr(name);
        return a ? Variant::mkObj(a) : Variant::mkObj(nullptr);
    }
    if (n == "setattribute") {
        std::string name = toStr(args[0]);
        std::string val = toStr(args[1]);
        auto a = findAttr(name);
        if (a) { a->value = val; return Variant(); }
        auto an = XmlNode::make(XML_ATTRIBUTE, name, val);
        an->parent = selfPtr;
        attrs.push_back(an);
        return Variant();
    }
    if (n == "removeattribute") {
        std::string name = toStr(args[0]);
        for (size_t i = 0; i < attrs.size(); i++)
            if (attrs[i]->name == name) { attrs.erase(attrs.begin() + i); return Variant(); }
        return Variant();
    }
    if (n == "appendchild") {
        auto c = asNode(args[0], 1);
        // detach from old parent
        if (auto p = c->parentPtr()) {
            for (size_t i = 0; i < p->children.size(); i++)
                if (p->children[i].get() == c.get()) { p->children.erase(p->children.begin() + i); break; }
        }
        c->parent = selfPtr;
        children.push_back(c);
        return Variant::mkObj(c);
    }
    if (n == "removechild") {
        auto c = asNode(args[0], 1);
        for (size_t i = 0; i < children.size(); i++)
            if (children[i].get() == c.get()) {
                children.erase(children.begin() + i);
                c->parent.reset();
                return Variant::mkObj(c);
            }
        throwErr(424, errDesc(424));
    }
    if (n == "replacechild") {
        auto nw = asNode(args[0], 1);
        auto old = asNode(args[1], 1);
        for (size_t i = 0; i < children.size(); i++)
            if (children[i].get() == old.get()) {
                if (auto p = nw->parentPtr()) {
                    for (size_t j = 0; j < p->children.size(); j++)
                        if (p->children[j].get() == nw.get()) { p->children.erase(p->children.begin() + j); break; }
                }
                nw->parent = selfPtr;
                children[i] = nw;
                old->parent.reset();
                return Variant::mkObj(old);
            }
        throwErr(424, errDesc(424));
    }
    if (n == "insertbefore") {
        auto nw = asNode(args[0], 1);
        bool hasRef = args.size() >= 2 && !(args[1].t == VT::Object && !args[1].obj);
        if (!hasRef) {
            std::vector<Variant> one;
            one.push_back(args[0]);
            return callMember("appendchild", one);
        }
        auto ref = asNode(args[1], 1);
        if (auto p = nw->parentPtr()) {
            for (size_t j = 0; j < p->children.size(); j++)
                if (p->children[j].get() == nw.get()) { p->children.erase(p->children.begin() + j); break; }
        }
        nw->parent = selfPtr;
        for (size_t i = 0; i < children.size(); i++)
            if (children[i].get() == ref.get()) { children.insert(children.begin() + i, nw); return Variant::mkObj(nw); }
        children.push_back(nw);
        return Variant::mkObj(nw);
    }
    if (n == "clonenode") {
        bool deep = args.empty() ? false : toBool(args[0]);
        auto c = XmlNode::make(type, name, value);
        if (type == XML_ELEMENT) c->attrs = attrs; // shallow copy of attr nodes (shared) — fine for read use
        if (deep) {
            for (auto& ch : children) {
                // deep clone
                std::function<XmlNodePtr(const XmlNodePtr&)> clone = [&](const XmlNodePtr& src) -> XmlNodePtr {
                    auto d = XmlNode::make(src->type, src->name, src->value);
                    d->attrs = src->attrs;
                    for (auto& gc : src->children) {
                        auto g = clone(gc);
                        g->parent = d;
                        d->children.push_back(g);
                    }
                    return d;
                };
                auto g = clone(ch);
                g->parent = c;
                c->children.push_back(g);
            }
        }
        return Variant::mkObj(c);
    }
    if (n == "haschildnodes") return Variant::mkBool(!children.empty());
    if (n == "selectnodes") return Variant::mkObj(makeList(xpathSelect(selfPtr, toStr(args[0]))));
    if (n == "selectsinglenode") {
        auto r = xpathSelect(selfPtr, toStr(args[0]));
        return r.empty() ? Variant::mkObj(nullptr) : Variant::mkObj(r[0]);
    }
    if (n == "getelementsbytagname") return Variant::mkObj(makeList(elementsByTagName(selfPtr, toStr(args[0]))));
    if (n == "transformnode") return Variant::mkStr(serialize());
    throwErr(438, errDesc(438));
}

// ---------- parse error ----------

struct XmlParseError : MapObject {
    XmlParseError() {
        kind = "IXMLDOMParseError";
        props["errorcode"] = Variant::mkInt(0);
        props["reason"] = Variant::mkStr("");
        props["line"] = Variant::mkInt(0);
        props["linepos"] = Variant::mkInt(0);
        props["column"] = Variant::mkInt(0); // alias
        props["srcText"] = Variant::mkStr("");
        props["filepos"] = Variant::mkInt(0);
        props["url"] = Variant::mkStr("");
    }
    void setMember(const std::string& n, const Variant& v) override {
        if (n == "column") { props["linepos"] = v; props["column"] = v; return; }
        if (n == "srctext") { props["srctext"] = v; return; }
        MapObject::setMember(n, v);
    }
    Variant getMember(const std::string& n) override {
        if (n == "srctext") return props["srctext"];
        if (n == "column") return props["linepos"];
        return MapObject::getMember(n);
    }
};

// ---------- document ----------

struct XmlDocument : VBObject {
    Interp* interp;
    XmlNodePtr doc;             // XML_DOCUMENT node
    std::shared_ptr<XmlParseError> parseError;
    bool async = false;
    bool preserveWhiteSpace = false;

    explicit XmlDocument(Interp* ip) : interp(ip) {
        doc = XmlNode::make(XML_DOCUMENT, "#document");
        parseError = std::make_shared<XmlParseError>();
    }

    XmlNodePtr rootElement() const {
        for (auto& c : doc->children) if (c->type == XML_ELEMENT) return c;
        return nullptr;
    }

    // ---- VBObject ----
    bool hasMember(const std::string& n) const override {
        static const char* mems[] = {"async","documentelement","parseerror","xml","preservewhitespace",
            "validateonparse","resolveexternals","url","readystate"};
        for (auto m : mems) if (n == m) return true;
        return hasMethod(n);
    }
    bool hasMethod(const std::string& n) const override {
        static const char* ms[] = {"load","loadxml","save","createelement","createtextnode","createcomment",
            "createattribute","getelementsbytagname","selectnodes","selectsinglenode","createnode","abort"};
        for (auto m : ms) if (n == m) return true;
        return false;
    }
    Variant getMember(const std::string& n) override;
    void setMember(const std::string& n, const Variant& v) override;
    Variant callMember(const std::string& n, std::vector<Variant>& args) override;
    std::string objectKind() const override { return "DOMDocument"; }
};

// real expat callbacks
struct ParseCtx {
    XmlNodePtr doc;
    std::vector<XmlNodePtr> stack;
    bool preserveWS;
    XML_Parser parser = nullptr;
    bool tooDeep = false;
    static constexpr size_t MAX_DEPTH = 512; // also bounds recursive tree destruction/traversal
};

static void XMLCALL xmlStartEl(void* ud, const XML_Char* name, const XML_Char** atts) {
    auto& c = *(ParseCtx*)ud;
    // nesting-depth cap: expat itself does not limit element depth, and our
    // tree is shared_ptr-based (recursive destruction) — abort the parse
    if (c.stack.size() >= ParseCtx::MAX_DEPTH) {
        c.tooDeep = true;
        if (c.parser) XML_StopParser(c.parser, XML_FALSE);
        return;
    }
    auto el = XmlNode::make(XML_ELEMENT, name);
    el->parent = c.stack.back();
    for (int i = 0; atts[i]; i += 2) {
        auto a = XmlNode::make(XML_ATTRIBUTE, atts[i], atts[i + 1]);
        a->parent = el;
        el->attrs.push_back(a);
    }
    c.stack.back()->children.push_back(el);
    c.stack.push_back(el);
}
static void XMLCALL xmlEndEl(void* ud, const XML_Char*) {
    auto& c = *(ParseCtx*)ud;
    if (c.stack.size() > 1) c.stack.pop_back();
}
static void XMLCALL xmlCharData(void* ud, const XML_Char* s, int len) {
    auto& c = *(ParseCtx*)ud;
    std::string text(s, (size_t)len);
    if (!c.preserveWS) {
        bool wsOnly = std::all_of(text.begin(), text.end(), [](char ch){ return std::isspace((unsigned char)ch); });
        if (wsOnly) return;
    }
    auto& kids = c.stack.back()->children;
    if (!kids.empty() && (kids.back()->type == XML_TEXT || kids.back()->type == XML_CDATA)) {
        kids.back()->value += text;
        return;
    }
    auto t = XmlNode::make(XML_TEXT, "#text", text);
    t->parent = c.stack.back();
    kids.push_back(t);
}
static void XMLCALL xmlComment(void* ud, const XML_Char* s) {
    auto& c = *(ParseCtx*)ud;
    auto cm = XmlNode::make(XML_COMMENT, "#comment", s);
    cm->parent = c.stack.back();
    c.stack.back()->children.push_back(cm);
}

static bool parseXml(XmlDocument& docObj, const std::string& xml) {
    ParseCtx ctx;
    ctx.doc = XmlNode::make(XML_DOCUMENT, "#document");
    ctx.stack.push_back(ctx.doc);
    ctx.preserveWS = docObj.preserveWhiteSpace;

    if (xml.size() > (size_t)INT_MAX) { // XML_Parse takes int
        docObj.parseError->props["errorcode"] = Variant::mkInt(-1);
        docObj.parseError->props["reason"] = Variant::mkStr("document too large");
        docObj.doc = ctx.doc;
        return false;
    }

    XML_Parser p = XML_ParserCreate(nullptr);
    ctx.parser = p;
    XML_SetUserData(p, &ctx);
    XML_SetElementHandler(p, xmlStartEl, xmlEndEl);
    XML_SetCharacterDataHandler(p, xmlCharData);
    XML_SetCommentHandler(p, xmlComment);
    enum XML_Status st = XML_Parse(p, xml.data(), (int)xml.size(), 1);
    if (st == XML_STATUS_ERROR) {
        enum XML_Error ec = XML_GetErrorCode(p);
        docObj.parseError->props["errorcode"] = Variant::mkInt(-(int)ec);
        docObj.parseError->props["reason"] = Variant::mkStr(
            ctx.tooDeep ? "maximum element nesting depth (512) exceeded" : XML_ErrorString(ec));
        docObj.parseError->props["line"] = Variant::mkLong((long long)XML_GetCurrentLineNumber64(p));
        docObj.parseError->props["linepos"] = Variant::mkLong((long long)XML_GetCurrentColumnNumber64(p));
        docObj.parseError->props["column"] = Variant::mkLong((long long)XML_GetCurrentColumnNumber64(p));
        XML_ParserFree(p);
        docObj.doc = ctx.doc;
        return false;
    }
    XML_ParserFree(p);
    docObj.doc = ctx.doc;
    docObj.parseError->props["errorcode"] = Variant::mkInt(0);
    docObj.parseError->props["reason"] = Variant::mkStr("");
    return true;
}

Variant XmlDocument::getMember(const std::string& n) {
    if (n == "async") return Variant::mkBool(async);
    if (n == "preservewhitespace") return Variant::mkBool(preserveWhiteSpace);
    if (n == "validateonparse" || n == "resolveexternals") return Variant::mkBool(true);
    if (n == "documentelement") { auto r = rootElement(); return r ? Variant::mkObj(r) : Variant::mkObj(nullptr); }
    if (n == "parseerror") return Variant::mkObj(parseError);
    if (n == "xml") return Variant::mkStr(doc->serialize());
    if (n == "url") return Variant::mkStr("");
    if (n == "readystate") return Variant::mkInt(4);
    throwErr(438, errDesc(438));
}

void XmlDocument::setMember(const std::string& n, const Variant& v) {
    if (n == "async") { async = toBool(v); return; }
    if (n == "preservewhitespace") { preserveWhiteSpace = toBool(v); return; }
    if (n == "validateonparse" || n == "resolveexternals") return;
    if (n == "documentelement") {
        // setting documentElement replaces the root element
        auto el = asNode(v, 1);
        if (el->type != XML_ELEMENT) throwErr(13, errDesc(13));
        doc->children.clear();
        el->parent = doc;
        doc->children.push_back(el);
        return;
    }
    throwErr(438, errDesc(438));
}

Variant XmlDocument::callMember(const std::string& n, std::vector<Variant>& args) {
    if (n == "loadxml") {
        std::string s = toStr(args[0]);
        return Variant::mkBool(parseXml(*this, s));
    }
    if (n == "load") {
        std::string path = toStr(args[0]);
        std::error_code ec;
        if (!fs::is_regular_file(path, ec)) {
            parseError->props["errorcode"] = Variant::mkLong(-2147024894); // 0x80070002 file not found
            parseError->props["reason"] = Variant::mkStr("The system cannot locate the object specified.");
            return Variant::mkBool(false);
        }
        std::ifstream in(path, std::ios::binary);
        std::ostringstream ss;
        ss << in.rdbuf();
        return Variant::mkBool(parseXml(*this, ss.str()));
    }
    if (n == "save") {
        std::string path = toStr(args[0]);
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out) throwErr(70, "Permission denied");
        std::string x = doc->serialize();
        out.write(x.data(), (std::streamsize)x.size());
        return Variant();
    }
    if (n == "createelement") return Variant::mkObj(XmlNode::make(XML_ELEMENT, toStr(args[0])));
    if (n == "createtextnode") return Variant::mkObj(XmlNode::make(XML_TEXT, "#text", toStr(args[0])));
    if (n == "createcomment") return Variant::mkObj(XmlNode::make(XML_COMMENT, "#comment", toStr(args[0])));
    if (n == "createattribute") return Variant::mkObj(XmlNode::make(XML_ATTRIBUTE, toStr(args[0])));
    if (n == "createnode") {
        std::string t = lower(toStr(args[0]));
        std::string name = args.size() > 1 ? toStr(args[1]) : "";
        if (t == "element") return Variant::mkObj(XmlNode::make(XML_ELEMENT, name));
        if (t == "text") return Variant::mkObj(XmlNode::make(XML_TEXT, "#text", name));
        if (t == "comment") return Variant::mkObj(XmlNode::make(XML_COMMENT, "#comment", name));
        if (t == "attribute") return Variant::mkObj(XmlNode::make(XML_ATTRIBUTE, name));
        throwErr(5, errDesc(5));
    }
    if (n == "getelementsbytagname") return Variant::mkObj(makeList(elementsByTagName(doc, toStr(args[0]))));
    if (n == "selectnodes") return Variant::mkObj(makeList(xpathSelect(doc, toStr(args[0]))));
    if (n == "selectsinglenode") {
        auto r = xpathSelect(doc, toStr(args[0]));
        return r.empty() ? Variant::mkObj(nullptr) : Variant::mkObj(r[0]);
    }
    if (n == "abort") return Variant();
    throwErr(438, errDesc(438));
}

std::shared_ptr<VBObject> makeDomDocument(Interp& interp) {
    auto d = std::make_shared<XmlDocument>(&interp);
    return d;
}

std::shared_ptr<VBObject> makeDomDocumentFromXML(Interp& interp, const std::string& xml, bool& ok) {
    auto d = std::make_shared<XmlDocument>(&interp);
    ok = parseXml(*d, xml);
    return d;
}
