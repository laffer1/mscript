// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 Lucas Holt. All rights reserved.

// regexp.cpp — VBScript.RegExp on the system libc POSIX regex (<regex.h>).
//
// VBScript (JScript-style) patterns are translated to POSIX ERE:
//   \d \D \w \W \s \S  -> character classes
//   \n \r \t \f \v \a   -> literal control characters
//   (?:...)            -> (...) with a group-index map (VBScript group k maps
//                         to the k-th *capturing* POSIX group)
//   .                  -> [^\n] when Multiline is off (POSIX dot matches \n;
//                         with REG_NEWLINE, dot already excludes \n)
//   Multiline on       -> REG_NEWLINE (^ $ match at line boundaries)
//   IgnoreCase         -> REG_ICASE
// Unsupported (clear runtime error at compile time): backreferences (\1),
// non-greedy quantifiers (*? +? ??), lookaround (?= (?! (?<= (?<!), named
// groups (?<name>...), and \b (the BSD libc regex has no word boundary).
// REG_STARTEND is used for global matching and offsets.

#include "regexp.h"

#include <regex.h>

#include <string>
#include <vector>

namespace {

struct RegexData {
    std::string pattern;
    bool global = false;
    bool ignoreCase = false;
    bool multiline = false;

    bool compiled = false;
    regex_t re {};
    std::vector<int> vbsToPosix; // vbs group (1-based) -> posix group index
    int nsub = 0;
};

[[noreturn]] void badPattern(const std::string& why) {
    throwErr(5, "Invalid regular expression: " + why);
}

// Translate a VBScript pattern to POSIX ERE. Fills vbsToPosix (index 0 unused).
std::string translate(const std::string& pat, bool multiline, std::vector<int>& vbsToPosix) {
    std::string out;
    out.reserve(pat.size() * 2);
    vbsToPosix.clear();
    vbsToPosix.push_back(0); // group 0 = whole match
    int posixGroups = 0;
    bool inClass = false;
    bool classStart = false; // just after '[' (or '[^') — ']' is literal there
    size_t i = 0;
    auto emitClassChar = [&](char c) { out += c; };
    while (i < pat.size()) {
        char c = pat[i];
        if (c == '\\' && i + 1 < pat.size()) {
            char e = pat[i + 1];
            i += 2;
            if (inClass) {
                classStart = false;
                switch (e) {
                case 'd': out += "0-9"; break;
                case 'w': out += "A-Za-z0-9_"; break;
                case 's': out += " \t\n\r\x0b\x0c"; break;
                case 'D': case 'W': case 'S': badPattern(std::string("\\") + e + " inside a character class is not supported"); break;
                case 'n': emitClassChar('\n'); break;
                case 'r': emitClassChar('\r'); break;
                case 't': emitClassChar('\t'); break;
                case 'f': emitClassChar('\f'); break;
                case 'v': emitClassChar('\v'); break;
                case 'a': emitClassChar('\a'); break;
                case 'b': badPattern("\\b is not supported by the system regex"); break;
                default:
                    if (e >= '1' && e <= '9') badPattern("backreferences are not supported by the system regex");
                    out += e; // escaped char inside class is literal
                }
                continue;
            }
            switch (e) {
            case 'd': out += "[0-9]"; break;
            case 'D': out += "[^0-9]"; break;
            case 'w': out += "[A-Za-z0-9_]"; break;
            case 'W': out += "[^A-Za-z0-9_]"; break;
            case 's': out += "[ \t\n\r\x0b\x0c]"; break;
            case 'S': out += "[^ \t\n\r\x0b\x0c]"; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'f': out += '\f'; break;
            case 'v': out += '\v'; break;
            case 'a': out += '\a'; break;
            case 'b': badPattern("\\b is not supported by the system regex"); break;
            default:
                if (e >= '1' && e <= '9') badPattern("backreferences are not supported by the system regex");
                // escape of a metacharacter stays escaped; anything else is a literal char
                if (std::string(".^$*+?()[]{}|\\").find(e) != std::string::npos) { out += '\\'; out += e; }
                else out += e;
            }
            continue;
        }
        if (inClass) {
            if (c == ']' && !classStart) { inClass = false; out += c; i++; continue; }
            classStart = false;
            out += c;
            i++;
            continue;
        }
        if (c == '[') {
            inClass = true;
            classStart = true;
            out += c;
            i++;
            if (i < pat.size() && pat[i] == '^') { out += '^'; i++; classStart = true; }
            continue;
        }
        if (c == '(') {
            // group openers
            if (i + 1 < pat.size() && pat[i + 1] == '?') {
                if (i + 2 < pat.size() && pat[i + 2] == ':') {
                    out += '('; // non-capturing: becomes capturing in POSIX; remapped via vbsToPosix
                    posixGroups++;
                    i += 3;
                    continue;
                }
                badPattern("lookaround/named groups (?...) are not supported");
            }
            out += c;
            posixGroups++;
            vbsToPosix.push_back(posixGroups);
            i++;
            continue;
        }
        if ((c == '*' || c == '+' || c == '?') && i + 1 < pat.size() && pat[i + 1] == '?')
            badPattern("non-greedy quantifiers are not supported by the system regex");
        if (c == '.' && !multiline) { out += "[^\n]"; i++; continue; }
        out += c;
        i++;
    }
    if (inClass) badPattern("unterminated character class");
    return out;
}

void compile(RegexData& d) {
    if (d.compiled) { regfree(&d.re); d.compiled = false; }
    if (d.pattern.empty()) badPattern("empty pattern");
    std::vector<int> map;
    std::string ere = translate(d.pattern, d.multiline, map);
    int flags = REG_EXTENDED;
    if (d.ignoreCase) flags |= REG_ICASE;
    if (d.multiline) flags |= REG_NEWLINE;
    regex_t re;
    int rc = regcomp(&re, ere.c_str(), flags);
    if (rc != 0) {
        char errbuf[256];
        regerror(rc, &re, errbuf, sizeof errbuf);
        badPattern(std::string(errbuf) + " (pattern '" + d.pattern + "')");
    }
    d.re = re;
    d.vbsToPosix = std::move(map);
    d.nsub = (int)re.re_nsub;
    d.compiled = true;
}

struct MatchData {
    std::string value;
    long long firstIndex = 0;
    long long length = 0;
    std::vector<Variant> subMatches; // Empty for non-participating groups
};

struct SubMatches : VBObject {
    std::vector<Variant> items;
    bool hasMember(const std::string& n) const override { return n == "count" || n == "item"; }
    bool hasMethod(const std::string& n) const override { return n == "item"; }
    Variant getMember(const std::string& n) override {
        if (n == "count") return Variant::mkLong((long long)items.size());
        throwErr(438, errDesc(438));
    }
    Variant callMember(const std::string& n, std::vector<Variant>& args) override {
        if (n == "item") {
            long long i = args.empty() ? 0 : toInt64(args[0]);
            if (i < 0 || (size_t)i >= items.size()) throwErr(9, errDesc(9));
            return items[(size_t)i];
        }
        throwErr(438, errDesc(438));
    }
    Variant invoke(const std::vector<Variant>& args) override {
        long long i = args.empty() ? 0 : toInt64(args[0]);
        if (i < 0 || (size_t)i >= items.size()) throwErr(9, errDesc(9));
        return items[(size_t)i];
    }
    std::vector<Variant> enumItems() override { return items; }
    std::string objectKind() const override { return "ISubMatches"; }
};

struct Match : VBObject {
    std::shared_ptr<MatchData> d;
    explicit Match(std::shared_ptr<MatchData> data) : d(std::move(data)) {}
    bool hasMember(const std::string& n) const override {
        return n == "firstindex" || n == "length" || n == "value" || n == "submatches";
    }
    Variant getMember(const std::string& n) override {
        if (n == "firstindex") return Variant::mkLong(d->firstIndex);
        if (n == "length") return Variant::mkLong(d->length);
        if (n == "value") return Variant::mkStr(d->value);
        if (n == "submatches") {
            auto s = std::make_shared<SubMatches>();
            s->items = d->subMatches;
            return Variant::mkObj(s);
        }
        throwErr(438, errDesc(438));
    }
    std::string objectKind() const override { return "IMatch2"; }
};

struct MatchCollection : VBObject {
    std::vector<std::shared_ptr<MatchData>> items;
    bool hasMember(const std::string& n) const override { return n == "count" || n == "item"; }
    bool hasMethod(const std::string& n) const override { return n == "item"; }
    Variant getMember(const std::string& n) override {
        if (n == "count") return Variant::mkLong((long long)items.size());
        throwErr(438, errDesc(438));
    }
    Variant callMember(const std::string& n, std::vector<Variant>& args) override {
        if (n == "item") {
            long long i = args.empty() ? 0 : toInt64(args[0]);
            if (i < 0 || (size_t)i >= items.size()) return Variant::mkObj(nullptr); // Nothing
            return Variant::mkObj(std::make_shared<Match>(items[(size_t)i]));
        }
        throwErr(438, errDesc(438));
    }
    Variant invoke(const std::vector<Variant>& args) override {
        long long i = args.empty() ? 0 : toInt64(args[0]);
        if (i < 0 || (size_t)i >= items.size()) throwErr(9, errDesc(9));
        return Variant::mkObj(std::make_shared<Match>(items[(size_t)i]));
    }
    std::vector<Variant> enumItems() override {
        std::vector<Variant> out;
        for (auto& it : items) out.push_back(Variant::mkObj(std::make_shared<Match>(it)));
        return out;
    }
    std::string objectKind() const override { return "IMatches2"; }
};

// One match found by regexec; builds MatchData with submatches mapped through vbsToPosix.
std::shared_ptr<MatchData> makeMatch(RegexData& d, const std::string& s,
                                     const regmatch_t* pm, int nmatch) {
    auto m = std::make_shared<MatchData>();
    m->firstIndex = pm[0].rm_so;
    m->length = pm[0].rm_eo - pm[0].rm_so;
    m->value = s.substr((size_t)pm[0].rm_so, (size_t)m->length);
    for (size_t g = 1; g < d.vbsToPosix.size(); g++) {
        int pg = d.vbsToPosix[g];
        if (pg < nmatch && pm[pg].rm_so >= 0)
            m->subMatches.push_back(Variant::mkStr(s.substr((size_t)pm[pg].rm_so,
                                                            (size_t)(pm[pg].rm_eo - pm[pg].rm_so))));
        else
            m->subMatches.push_back(Variant()); // Empty
    }
    return m;
}

std::vector<std::shared_ptr<MatchData>> findAll(RegexData& d, const std::string& s) {
    if (!d.compiled) compile(d);
    std::vector<std::shared_ptr<MatchData>> out;
    int nmatch = d.nsub + 1;
    std::vector<regmatch_t> pm((size_t)nmatch);
    if (!d.global) {
        if (regexec(&d.re, s.c_str(), (size_t)nmatch, pm.data(), 0) == 0)
            out.push_back(makeMatch(d, s, pm.data(), nmatch));
        return out;
    }
    size_t start = 0;
    while (start <= s.size()) {
        pm[0].rm_so = (regoff_t)start;
        pm[0].rm_eo = (regoff_t)s.size();
        int eflags = start > 0 ? REG_NOTBOL : 0;
        int rc = regexec(&d.re, s.c_str(), (size_t)nmatch, pm.data(), eflags | REG_STARTEND);
        if (rc != 0) break;
        out.push_back(makeMatch(d, s, pm.data(), nmatch));
        size_t next = (pm[0].rm_eo > (regoff_t)start) ? (size_t)pm[0].rm_eo : (size_t)pm[0].rm_eo + 1;
        if (next <= start) break;
        start = next;
    }
    return out;
}

// Expand $$, $&, $`, $' and $1..$99 in a replacement string.
std::string expandReplacement(const std::string& repl, const std::string& s,
                              const regmatch_t& whole, MatchData& m, RegexData& d) {
    std::string out;
    size_t i = 0;
    auto groupText = [&](int g) -> std::string {
        if (g < 1 || (size_t)g >= d.vbsToPosix.size()) return "";
        return m.subMatches[(size_t)g - 1].t == VT::Empty ? "" : toStr(m.subMatches[(size_t)g - 1]);
    };
    while (i < repl.size()) {
        char c = repl[i];
        if (c == '$' && i + 1 < repl.size()) {
            char e = repl[i + 1];
            if (e == '$') { out += '$'; i += 2; continue; }
            if (e == '&') { out += m.value; i += 2; continue; }
            if (e == '`') { out += s.substr(0, (size_t)whole.rm_so); i += 2; continue; }
            if (e == '\'') { out += s.substr((size_t)whole.rm_eo); i += 2; continue; }
            if (e >= '1' && e <= '9') {
                int g = e - '0';
                size_t j = i + 2;
                if (j < repl.size() && repl[j] >= '0' && repl[j] <= '9') {
                    int g2 = g * 10 + (repl[j] - '0');
                    if (g2 <= (int)d.vbsToPosix.size() - 1) { g = g2; j++; }
                }
                out += groupText(g);
                i = j;
                continue;
            }
        }
        out += c;
        i++;
    }
    return out;
}

struct RegExpObject : MapObject {
    std::shared_ptr<RegexData> d;
    explicit RegExpObject(std::shared_ptr<RegexData> data) : d(std::move(data)) {
        kind = "VBScript.RegExp";
    }
    bool hasMember(const std::string& n) const override {
        return n == "pattern" || n == "global" || n == "ignorecase" || n == "multiline" ||
               MapObject::hasMember(n);
    }
    bool hasMethod(const std::string& n) const override {
        return n == "test" || n == "execute" || n == "replace" || MapObject::hasMethod(n);
    }
    Variant getMember(const std::string& n) override {
        if (n == "pattern") return Variant::mkStr(d->pattern);
        if (n == "global") return Variant::mkBool(d->global);
        if (n == "ignorecase") return Variant::mkBool(d->ignoreCase);
        if (n == "multiline") return Variant::mkBool(d->multiline);
        return MapObject::getMember(n);
    }
    void setMember(const std::string& n, const Variant& v) override {
        if (n == "pattern") { d->pattern = toStr(v); d->compiled = false; return; }
        if (n == "global") { d->global = toBool(v); return; }
        if (n == "ignorecase") { d->ignoreCase = toBool(v); d->compiled = false; return; }
        if (n == "multiline") { d->multiline = toBool(v); d->compiled = false; return; }
        MapObject::setMember(n, v);
    }
};

} // namespace

std::shared_ptr<VBObject> makeRegExp(Interp&) {
    auto data = std::make_shared<RegexData>();
    auto o = std::make_shared<RegExpObject>(data);

    o->methods["test"] = [data](MapObject*, std::vector<Variant>& a) -> Variant {
        if (a.empty()) throwErr(450, errDesc(450));
        if (!data->compiled) compile(*data);
        return Variant::mkBool(regexec(&data->re, toStr(a[0]).c_str(), 0, nullptr, 0) == 0);
    };
    o->methods["execute"] = [data](MapObject*, std::vector<Variant>& a) -> Variant {
        if (a.empty()) throwErr(450, errDesc(450));
        auto matches = findAll(*data, toStr(a[0]));
        auto coll = std::make_shared<MatchCollection>();
        coll->items = std::move(matches);
        return Variant::mkObj(coll);
    };
    o->methods["replace"] = [data](MapObject*, std::vector<Variant>& a) -> Variant {
        if (a.size() < 2) throwErr(450, errDesc(450));
        std::string s = toStr(a[0]);
        std::string repl = toStr(a[1]);
        if (!data->compiled) compile(*data);
        std::string out;
        size_t last = 0;
        int nmatch = data->nsub + 1;
        std::vector<regmatch_t> pm((size_t)nmatch);
        size_t start = 0;
        while (start <= s.size()) {
            pm[0].rm_so = (regoff_t)start;
            pm[0].rm_eo = (regoff_t)s.size();
            int eflags = (start > 0 ? REG_NOTBOL : 0) | REG_STARTEND;
            if (regexec(&data->re, s.c_str(), (size_t)nmatch, pm.data(), eflags) != 0) break;
            auto m = makeMatch(*data, s, pm.data(), nmatch);
            out += s.substr(last, (size_t)pm[0].rm_so - last);
            out += expandReplacement(repl, s, pm[0], *m, *data);
            last = (size_t)pm[0].rm_eo;
            size_t next = (pm[0].rm_eo > pm[0].rm_so) ? (size_t)pm[0].rm_eo : (size_t)pm[0].rm_eo + 1;
            if (!data->global) break;
            if (next <= start) break;
            start = next;
        }
        out += s.substr(last);
        return Variant::mkStr(out);
    };

    return o;
}
