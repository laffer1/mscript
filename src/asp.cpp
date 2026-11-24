// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 Lucas Holt. All rights reserved.

// asp.cpp — classic-ASP extension: .asp templates (mixed HTML / <% %> code),
// the CGI object model, and SQLite-backed Session/Application state.
// State lives in /var/db/mscript (override with MSCRIPT_ASP_STATE_DB).
#include "asp.h"

#include "interpreter.h"
#include "objects.h"
#include "parser.h"
#include "lexer.h"

#include <sqlite3.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <ctime>
#include <format>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>
#include <unistd.h>

namespace {

// ---------- template transform ----------

void emitLiteral(std::string& out, const std::string& lit) {
    if (lit.empty()) return;
    size_t pos = 0, n = lit.size();
    while (pos < n) {
        size_t nl = lit.find('\n', pos);
        size_t end = (nl == std::string::npos) ? n : nl;
        std::string line = lit.substr(pos, end - pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        out += "Response.Write \"";
        for (char c : line) {
            if (c == '"') out += "\"\"";
            else out += c;
        }
        out += '"';
        if (nl != std::string::npos) out += " & vbCrLf";
        out += '\n';
        pos = (nl == std::string::npos) ? n : nl + 1;
    }
}

} // namespace

std::string aspToVbs(const std::string& src) {
    std::string out;
    out.reserve(src.size() + src.size() / 3 + 16);
    std::string lit;
    bool skipNl = false; // a <%@ %> directive also swallows the following newline
    size_t i = 0, n = src.size();
    while (i < n) {
        if (src.compare(i, 2, "<%") == 0) {
            if (skipNl) {
                // swallow the newline(s) immediately after the directive line
                while (!lit.empty() && (lit.front() == '\n' || lit.front() == '\r'))
                    lit.erase(lit.begin());
            }
            skipNl = false;
            emitLiteral(out, lit);
            lit.clear();
            size_t close = src.find("%>", i + 2);
            if (close == std::string::npos) close = n; // unterminated: rest is code
            std::string code = src.substr(i + 2, close - (i + 2));
            if (!code.empty() && code[0] == '@') {
                skipNl = true; // <%@ ... %> directive: dropped
            } else if (!code.empty() && code[0] == '=') {
                out += "Response.Write (" + code.substr(1) + ")\n";
            } else {
                out += code;
                if (close >= n || src[close - 1] != '\n') out += '\n';
            }
            i = (close >= n) ? n : close + 2;
        } else {
            lit += src[i++];
        }
    }
    emitLiteral(out, lit);
    return out;
}

namespace {

// ---------- URL / form decoding ----------

std::string urlDecode(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        char c = s[i];
        if (c == '+') out += ' ';
        else if (c == '%' && i + 2 < s.size() && std::isxdigit((unsigned char)s[i + 1]) &&
                 std::isxdigit((unsigned char)s[i + 2])) {
            auto hv = [](char h) -> int {
                return h >= '0' && h <= '9' ? h - '0'
                       : h >= 'a' && h <= 'f' ? h - 'a' + 10
                       : h - 'A' + 10;
            };
            out += (char)(hv(s[i + 1]) * 16 + hv(s[i + 2]));
            i += 2;
        } else out += c;
    }
    return out;
}

std::string urlEncode(const std::string& s) {
    std::string out;
    char buf[4];
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out += (char)c;
        else {
            std::snprintf(buf, sizeof buf, "%%%02X", c);
            out += buf;
        }
    }
    return out;
}

std::map<std::string, std::string> parsePairs(const std::string& s) {
    std::map<std::string, std::string> m;
    size_t pos = 0;
    while (pos <= s.size()) {
        size_t amp = s.find('&', pos);
        std::string pair = s.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos);
        if (!pair.empty()) {
            size_t eq = pair.find('=');
            std::string k = lower(urlDecode(eq == std::string::npos ? pair : pair.substr(0, eq)));
            std::string v = eq == std::string::npos ? "" : urlDecode(pair.substr(eq + 1));
            m[k] = v;
        }
        if (amp == std::string::npos) break;
        pos = amp + 1;
    }
    return m;
}

// ---------- Response state ----------

struct AspResponse {
    std::string body;
    std::string status = "200 OK";
    std::string contentType = "text/html";
    std::vector<std::pair<std::string, std::string>> headers; // extra headers
    std::vector<std::string> cookies;                          // Set-Cookie values
    bool buffer = true;
    bool headersSent = false;
    bool ended = false;
};

// one interpreter per CGI process
std::shared_ptr<AspResponse> g_resp;

void ensureHeaders() {
    if (!g_resp || g_resp->headersSent) return;
    g_resp->headersSent = true;
    bool cgi = std::getenv("REQUEST_METHOD") != nullptr;
    if (!cgi) return; // local run: body only
    std::string h;
    if (g_resp->status != "200 OK") h += "Status: " + g_resp->status + "\r\n";
    h += "Content-Type: " + g_resp->contentType + "\r\n";
    for (auto& c : g_resp->cookies) h += "Set-Cookie: " + c + "\r\n";
    for (auto& kv : g_resp->headers) h += kv.first + ": " + kv.second + "\r\n";
    h += "\r\n";
    std::fwrite(h.data(), 1, h.size(), stdout);
}

void responseBody(const std::string& s) {
    if (!g_resp || g_resp->ended) return;
    if (g_resp->buffer) g_resp->body += s;
    else {
        ensureHeaders();
        std::fwrite(s.data(), 1, s.size(), stdout);
        std::fflush(stdout);
    }
}

// ---------- key/value collection (Request.QueryString, Form, ...) ----------

std::shared_ptr<MapObject> makeColl(std::map<std::string, std::string> data, const std::string& kind) {
    auto c = std::make_shared<MapObject>();
    c->kind = kind;
    c->props["count"] = Variant::mkLong((long long)data.size());
    c->onInvoke = [data](MapObject*, std::vector<Variant>& a) -> Variant {
        if (a.empty()) throwErr(450, errDesc(450));
        auto it = data.find(lower(toStr(a[0])));
        return Variant::mkStr(it == data.end() ? "" : it->second);
    };
    c->onEnum = [data](MapObject*) -> std::vector<Variant> {
        std::vector<Variant> out;
        for (auto& kv : data) out.push_back(Variant::mkStr(kv.first));
        return out;
    };
    return c;
}

// ---------- SQLite state store (Session / Application) ----------

sqlite3* stateDb() {
    static sqlite3* db = []() -> sqlite3* {
        const char* env = std::getenv("MScript_ASP_STATE_DB");
        std::string dir = "/var/db/mscript";
        if (env && *env) dir = std::filesystem::path(env).parent_path().string();
        std::error_code ec;
        std::filesystem::create_directories(dir, ec); // may fail w/o perms; fall back below
        std::string path = env && *env ? env : dir + "/asp-state.db";
        sqlite3* d = nullptr;
        if (sqlite3_open(path.c_str(), &d) != SQLITE_OK) {
            if (d) sqlite3_close(d);
            path = "/tmp/mscript-asp-state.db";
            if (sqlite3_open(path.c_str(), &d) != SQLITE_OK) {
                if (d) sqlite3_close(d);
                return nullptr;
            }
        }
        sqlite3_exec(d,
            "CREATE TABLE IF NOT EXISTS app(k TEXT PRIMARY KEY, v TEXT);"
            "CREATE TABLE IF NOT EXISTS sessions(sid TEXT, k TEXT, v TEXT, PRIMARY KEY(sid, k));"
            "CREATE TABLE IF NOT EXISTS session_meta(sid TEXT PRIMARY KEY, last REAL);",
            nullptr, nullptr, nullptr);
        return d;
    }();
    return db;
}

std::string stateGet(const std::string& table, const std::string& a, const std::string& b) {
    sqlite3* db = stateDb();
    if (!db) return "";
    sqlite3_stmt* st = nullptr;
    std::string sql = table == "app"
        ? "SELECT v FROM app WHERE k=?1"
        : "SELECT v FROM sessions WHERE sid=?1 AND k=?2";
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) return "";
    sqlite3_bind_text(st, 1, a.c_str(), -1, SQLITE_TRANSIENT);
    if (table != "app") sqlite3_bind_text(st, 2, b.c_str(), -1, SQLITE_TRANSIENT);
    std::string out;
    if (sqlite3_step(st) == SQLITE_ROW) {
        const unsigned char* t = sqlite3_column_text(st, 0);
        if (t) out = (const char*)t;
    }
    sqlite3_finalize(st);
    return out;
}

void stateSet(const std::string& table, const std::string& a, const std::string& b, const std::string& v) {
    sqlite3* db = stateDb();
    if (!db) return;
    sqlite3_stmt* st = nullptr;
    std::string sql = table == "app"
        ? "INSERT OR REPLACE INTO app(k, v) VALUES(?1, ?2)"
        : "INSERT OR REPLACE INTO sessions(sid, k, v) VALUES(?1, ?2, ?3)";
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) return;
    sqlite3_bind_text(st, 1, a.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, table == "app" ? v.c_str() : b.c_str(), -1, SQLITE_TRANSIENT);
    if (table != "app") sqlite3_bind_text(st, 3, v.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(st);
    sqlite3_finalize(st);
}

void stateRemoveAll(const std::string& sid) {
    sqlite3* db = stateDb();
    if (!db) return;
    sqlite3_exec(db, ("DELETE FROM sessions WHERE sid='" + sid + "';").c_str(), nullptr, nullptr, nullptr);
    sqlite3_exec(db, ("DELETE FROM session_meta WHERE sid='" + sid + "';").c_str(), nullptr, nullptr, nullptr);
}

void sessionTouch(const std::string& sid) {
    sqlite3* db = stateDb();
    if (!db) return;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db, "INSERT OR REPLACE INTO session_meta(sid, last) VALUES(?1, ?2)", -1, &st, nullptr) != SQLITE_OK) return;
    sqlite3_bind_text(st, 1, sid.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(st, 2, (double)std::time(nullptr));
    sqlite3_step(st);
    sqlite3_finalize(st);
}

bool sessionExpired(const std::string& sid, long long timeoutMin) {
    sqlite3* db = stateDb();
    if (!db) return false;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT last FROM session_meta WHERE sid=?1", -1, &st, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(st, 1, sid.c_str(), -1, SQLITE_TRANSIENT);
    bool expired = true;
    if (sqlite3_step(st) == SQLITE_ROW) {
        double last = sqlite3_column_double(st, 0);
        expired = (std::time(nullptr) - (time_t)last) > timeoutMin * 60;
    }
    sqlite3_finalize(st);
    return expired;
}

// serialize a Variant to JSON text and back (small self-contained codec, so
// this extension does not depend on the JSON object extension)
void serJson(std::string& out, const Variant& v, int depth) {
    if (depth > 64) throwErr(1001, "ASP state: value too deep");
    switch (v.t) {
        case VT::Empty: case VT::Null: out += "null"; break;
        case VT::Bool: out += v.b ? "true" : "false"; break;
        case VT::Int: case VT::Long: case VT::Byte:
            out += std::to_string(v.ll); break;
        case VT::Currency: out += std::format("{}", (double)v.ll / (double)CY_SCALE); break;
        case VT::Single: case VT::Double: {
            if (!std::isfinite(v.d)) { out += "null"; break; }
            out += std::format("{}", v.d);
            break;
        }
        case VT::Date: {
            // ISO 8601 UTC, like JS Date#toJSON
            double days = std::floor(v.d), frac = v.d - days;
            long long unixDays = (long long)days - VB_EPOCH_OFF;
            long long y; unsigned m, d;
            civilFromDays(unixDays, y, m, d);
            long long totalMs = (long long)std::llround(frac * 86400000.0);
            long long secs = totalMs / 1000, ms = totalMs % 1000;
            if (secs >= 86400) { secs -= 86400; unixDays++; civilFromDays(unixDays, y, m, d); }
            char buf[40];
            std::snprintf(buf, sizeof buf, "%04lld-%02u-%02uT%02lld:%02lld:%02lld.%03lldZ",
                          y, m, d, secs / 3600, (secs / 60) % 60, secs % 60, ms);
            out += '"';
            out += buf;
            out += '"';
            break;
        }
        case VT::String: {
            out += '"';
            for (unsigned char c : v.s) {
                switch (c) {
                    case '"': out += "\\\""; break;
                    case '\\': out += "\\\\"; break;
                    case '\n': out += "\\n"; break;
                    case '\r': out += "\\r"; break;
                    case '\t': out += "\\t"; break;
                    default:
                        if (c < 0x20) {
                            char b[8];
                            std::snprintf(b, sizeof b, "\\u%04x", c);
                            out += b;
                        } else out += (char)c;
                }
            }
            out += '"';
            break;
        }
        case VT::Array: {
            out += '[';
            if (v.arr && v.arr->allocated) {
                for (size_t i = 0; i < v.arr->data.size(); i++) {
                    if (i) out += ',';
                    serJson(out, v.arr->data[i], depth + 1);
                }
            }
            out += ']';
            break;
        }
        case VT::Object: {
            if (!v.obj) { out += "null"; break; }
            auto* m = dynamic_cast<MapObject*>(v.obj.get());
            auto* ci = dynamic_cast<ClassInstance*>(v.obj.get());
            out += '{';
            bool first = true;
            auto emitProps = [&](auto& props) {
                for (auto& kv : props) {
                    if (!first) out += ',';
                    first = false;
                    out += '"';
                    for (unsigned char c : kv.first) {
                        if (c == '"' || c == '\\') { out += '\\'; out += (char)c; }
                        else if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); out += b; }
                        else out += (char)c;
                    }
                    out += "\":";
                    serJson(out, kv.second, depth + 1);
                }
            };
            if (m) emitProps(m->props);
            else if (ci) emitProps(ci->fields.vars);
            else { out += '}'; break; }
            out += '}';
            break;
        }
    }
}

struct JsonParser {
    const std::string& s;
    size_t i = 0;
    explicit JsonParser(const std::string& str) : s(str) {}
    void ws() { while (i < s.size() && std::isspace((unsigned char)s[i])) i++; }
    bool eat(char c) { ws(); if (i < s.size() && s[i] == c) { i++; return true; } return false; }
    Variant parse() {
        ws();
        if (i >= s.size()) throwErr(1001, "ASP state: empty value");
        char c = s[i];
        if (c == '{') {
            i++;
            auto m = std::make_shared<MapObject>();
            m->kind = "Object";
            ws();
            if (i < s.size() && s[i] == '}') { i++; return Variant::mkObj(m); }
            for (;;) {
                ws();
                if (i >= s.size() || s[i] != '"') throwErr(1001, "ASP state: bad object key");
                std::string k = parseString();
                if (!eat(':')) throwErr(1001, "ASP state: expected ':'");
                m->props[lower(k)] = parse();
                if (eat(',')) continue;
                if (eat('}')) break;
                throwErr(1001, "ASP state: expected ',' or '}'");
            }
            return Variant::mkObj(m);
        }
        if (c == '[') {
            i++;
            std::vector<Variant> items;
            ws();
            if (i < s.size() && s[i] == ']') { i++; }
            else {
                for (;;) {
                    items.push_back(parse());
                    if (eat(',')) continue;
                    if (eat(']')) break;
                    throwErr(1001, "ASP state: expected ',' or ']'");
                }
            }
            auto a = std::make_shared<VArray>();
            a->alloc({(long long)items.size()}, 0, false);
            for (size_t j = 0; j < items.size(); j++) a->data[j] = std::move(items[j]);
            return Variant::mkArr(a);
        }
        if (c == '"') return Variant::mkStr(parseString());
        if (s.compare(i, 4, "true") == 0) { i += 4; return Variant::mkBool(true); }
        if (s.compare(i, 5, "false") == 0) { i += 5; return Variant::mkBool(false); }
        if (s.compare(i, 4, "null") == 0) { i += 4; return Variant::mkNull(); }
        // number
        size_t start = i;
        if (i < s.size() && (s[i] == '-' || s[i] == '+')) i++;
        bool isDouble = false;
        while (i < s.size() && (std::isdigit((unsigned char)s[i]) || s[i] == '.' || s[i] == 'e' ||
                                 s[i] == 'E' || s[i] == '+' || s[i] == '-')) {
            if (s[i] == '.' || s[i] == 'e' || s[i] == 'E') isDouble = true;
            i++;
        }
        if (start == i) throwErr(1001, "ASP state: bad value");
        std::string num = s.substr(start, i - start);
        if (isDouble) return Variant::mkDouble(std::strtod(num.c_str(), nullptr));
        return Variant::mkInt(std::strtoll(num.c_str(), nullptr, 10));
    }
    std::string parseString() {
        if (i >= s.size() || s[i] != '"') throwErr(1001, "ASP state: expected string");
        i++;
        std::string out;
        while (i < s.size() && s[i] != '"') {
            char c = s[i++];
            if (c == '\\' && i < s.size()) {
                char e = s[i++];
                switch (e) {
                    case 'n': out += '\n'; break;
                    case 'r': out += '\r'; break;
                    case 't': out += '\t'; break;
                    case 'b': out += '\b'; break;
                    case 'f': out += '\f'; break;
                    case 'u': {
                        if (i + 4 > s.size()) throwErr(1001, "ASP state: bad \\u escape");
                        unsigned cp = 0;
                        for (int j = 0; j < 4; j++) {
                            char h = s[i++];
                            cp = cp * 16 + (h >= '0' && h <= '9' ? h - '0' : h >= 'a' && h <= 'f' ? h - 'a' + 10 : h - 'A' + 10);
                        }
                        // encode as UTF-8
                        if (cp < 0x80) out += (char)cp;
                        else if (cp < 0x800) { out += (char)(0xC0 | (cp >> 6)); out += (char)(0x80 | (cp & 0x3F)); }
                        else { out += (char)(0xE0 | (cp >> 12)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
                        break;
                    }
                    default: out += e;
                }
            } else out += c;
        }
        if (i >= s.size()) throwErr(1001, "ASP state: unterminated string");
        i++;
        return out;
    }
};

std::string toJsonText(Interp& ip, const Variant& v) {
    (void)ip;
    std::string out;
    serJson(out, v, 0);
    return out;
}

Variant fromJsonText(Interp& ip, const std::string& s) {
    (void)ip;
    if (s.empty()) return Variant();
    try {
        return JsonParser(s).parse();
    } catch (VBSError&) {
        return Variant::mkStr(s); // not JSON: return raw string
    }
}

std::string newSessionId(Interp& ip) {
    char buf[17];
    for (int i = 0; i < 16; i++) std::snprintf(buf + i, 2, "%x", (int)(ip.rng() & 0xF));
    buf[16] = 0;
    return std::string(buf);
}

// stateful collection object backed by the sqlite store
std::shared_ptr<MapObject> makeStateColl(Interp& ip, bool isSession, const std::string& sid,
                                         std::shared_ptr<long long> timeoutMin) {
    auto c = std::make_shared<MapObject>();
    c->kind = isSession ? "Session" : "Application";
    if (isSession) {
        c->props["sessionid"] = Variant::mkStr(sid);
        c->props["timeout"] = Variant::mkLong(*timeoutMin);
    }
    auto keyOf = [](std::vector<Variant>& a) -> std::string {
        if (a.empty()) throwErr(450, errDesc(450));
        return lower(toStr(a[0]));
    };
    c->onInvoke = [&, isSession, sid, timeoutMin](MapObject*, std::vector<Variant>& a) -> Variant {
        std::string k = keyOf(a);
        if (isSession) {
            if (sessionExpired(sid, *timeoutMin)) { stateRemoveAll(sid); return Variant(); }
            sessionTouch(sid);
        }
        return fromJsonText(ip, stateGet(isSession ? "sessions" : "app", sid, k));
    };
    c->onAssignInvoke = [&, isSession, sid, timeoutMin](MapObject*, std::vector<Variant>& a, const Variant& v) {
        std::string k = keyOf(a);
        if (isSession) sessionTouch(sid);
        stateSet(isSession ? "sessions" : "app", sid, k, toJsonText(ip, v));
    };
    if (isSession) {
        c->methods["abandon"] = [](MapObject*, std::vector<Variant>&) -> Variant { return Variant(); };
        // abandon needs sid: wrap below
    }
    return c;
}

} // namespace

void registerAspObjects(Interp& ip, bool cgi) {
    (void)cgi;
    // ---- Response ----
    g_resp = std::make_shared<AspResponse>();
    auto resp = std::make_shared<MapObject>();
    resp->kind = "Response";
    ip.responseObj = resp;
    resp->props["buffer"] = Variant::mkBool(true);
    resp->props["contenttype"] = Variant::mkStr("text/html");
    resp->props["status"] = Variant::mkStr("200 OK");
    resp->methods["write"] = [](MapObject*, std::vector<Variant>& a) -> Variant {
        for (auto& v : a) responseBody(toStr(v));
        return Variant();
    };
    resp->methods["binarywrite"] = [](MapObject*, std::vector<Variant>& a) -> Variant {
        for (auto& v : a) {
            if (v.t == VT::Array && v.arr)
                for (auto& e : v.arr->data) {
                    std::string s = toStr(e);
                    responseBody(s);
                }
            else responseBody(toStr(v));
        }
        return Variant();
    };
    resp->methods["flush"] = [](MapObject*, std::vector<Variant>&) -> Variant {
        if (g_resp) {
            ensureHeaders();
            std::fwrite(g_resp->body.data(), 1, g_resp->body.size(), stdout);
            std::fflush(stdout);
            g_resp->body.clear();
        }
        return Variant();
    };
    resp->methods["clear"] = [](MapObject*, std::vector<Variant>&) -> Variant {
        if (g_resp) g_resp->body.clear();
        return Variant();
    };
    resp->methods["end"] = [](MapObject*, std::vector<Variant>&) -> Variant {
        if (g_resp) g_resp->ended = true;
        throw EndSig{};
    };
    resp->methods["redirect"] = [](MapObject*, std::vector<Variant>& a) -> Variant {
        if (a.empty()) throwErr(450, errDesc(450));
        if (g_resp) {
            g_resp->status = "302 Found";
            g_resp->headers.push_back({"Location", toStr(a[0])});
            g_resp->ended = true;
        }
        throw EndSig{};
    };
    resp->methods["addheader"] = [](MapObject*, std::vector<Variant>& a) -> Variant {
        if (a.size() < 2) throwErr(450, errDesc(450));
        if (g_resp) g_resp->headers.push_back({toStr(a[0]), toStr(a[1])});
        return Variant();
    };
    // Response.X = v writes props via MapObject::setMember; flushAspResponse
    // syncs buffer/contenttype/status/expires back into g_resp.

    // Response.Cookies("name") = value
    auto cookies = std::make_shared<MapObject>();
    cookies->kind = "Cookies";
    cookies->onAssignInvoke = [](MapObject*, std::vector<Variant>& a, const Variant& v) {
        if (a.empty() || !g_resp) return;
        g_resp->cookies.push_back(toStr(a[0]) + "=" + urlEncode(toStr(v)) + "; path=/");
    };
    cookies->onInvoke = [](MapObject*, std::vector<Variant>& a) -> Variant {
        if (a.empty()) return Variant();
        // look in the request cookies
        const char* hc = std::getenv("HTTP_COOKIE");
        if (!hc) return Variant::mkStr("");
        auto m = parsePairs(hc);
        auto it = m.find(lower(toStr(a[0])));
        return Variant::mkStr(it == m.end() ? "" : it->second);
    };
    resp->props["cookies"] = Variant::mkObj(cookies);

    // ---- Request ----
    auto req = std::make_shared<MapObject>();
    req->kind = "Request";
    auto query = parsePairs(std::getenv("QUERY_STRING") ? std::getenv("QUERY_STRING") : "");
    req->props["querystring"] = Variant::mkObj(makeColl(query, "QueryString"));
    std::map<std::string, std::string> form;
    const char* rm = std::getenv("REQUEST_METHOD");
    const char* cl = std::getenv("CONTENT_LENGTH");
    const char* ct = std::getenv("CONTENT_TYPE");
    if (rm && !strcmp(rm, "POST") && cl && ct && strncmp(ct, "application/x-www-form-urlencoded", 33) == 0) {
        long long len = atoll(cl);
        if (len > 0 && len < (1LL << 24)) {
            std::string body((size_t)len, '\0');
            size_t got = std::fread(body.data(), 1, (size_t)len, stdin);
            body.resize(got);
            form = parsePairs(body);
        }
    }
    req->props["form"] = Variant::mkObj(makeColl(form, "Form"));
    req->props["cookies"] = Variant::mkObj(makeColl(parsePairs(std::getenv("HTTP_COOKIE") ? std::getenv("HTTP_COOKIE") : ""), "Cookies"));
    auto sv = std::make_shared<MapObject>();
    sv->kind = "ServerVariables";
    sv->onInvoke = [](MapObject*, std::vector<Variant>& a) -> Variant {
        if (a.empty()) throwErr(450, errDesc(450));
        const char* v = std::getenv(toStr(a[0]).c_str());
        return Variant::mkStr(v ? v : "");
    };
    req->props["servervariables"] = Variant::mkObj(sv);
    req->props["totalbytes"] = Variant::mkLong(cl ? atoll(cl) : 0);
    req->methods["binaryread"] = [](MapObject*, std::vector<Variant>& a) -> Variant {
        long long len = a.empty() ? -1 : toInt64(a[0]);
        const char* cl = std::getenv("CONTENT_LENGTH");
        long long total = cl ? atoll(cl) : 0;
        if (len < 0 || len > total) len = total;
        if (len <= 0) return Variant::mkArr(std::make_shared<VArray>());
        std::string body((size_t)len, '\0');
        size_t got = std::fread(body.data(), 1, (size_t)len, stdin);
        body.resize(got);
        auto arr = std::make_shared<VArray>();
        arr->alloc({(long long)body.size()}, 0, false);
        for (size_t i = 0; i < body.size(); i++) arr->data[i] = Variant::mkByte((unsigned char)body[i]);
        return Variant::mkArr(arr);
    };

    // ---- Server ----
    auto srv = std::make_shared<MapObject>();
    srv->kind = "Server";
    srv->props["scripttimeout"] = Variant::mkLong(90);
    srv->methods["createobject"] = [&ip](MapObject*, std::vector<Variant>& a) -> Variant {
        if (a.empty()) throwErr(450, errDesc(450));
        auto o = createHostObject(ip, toStr(a[0]));
        if (!o) throwErr(429, "ActiveX component can't create object: '" + toStr(a[0]) + "'");
        return Variant::mkObj(o);
    };
    srv->methods["htmlencode"] = [](MapObject*, std::vector<Variant>& a) -> Variant {
        if (a.empty()) return Variant::mkStr("");
        std::string s = toStr(a[0]), out;
        for (char c : s) {
            switch (c) {
                case '&': out += "&amp;"; break;
                case '<': out += "&lt;"; break;
                case '>': out += "&gt;"; break;
                case '"': out += "&quot;"; break;
                default: out += c;
            }
        }
        return Variant::mkStr(out);
    };
    srv->methods["urlencode"] = [](MapObject*, std::vector<Variant>& a) -> Variant {
        if (a.empty()) return Variant::mkStr("");
        return Variant::mkStr(urlEncode(toStr(a[0])));
    };
    srv->methods["mappath"] = [&ip](MapObject*, std::vector<Variant>& a) -> Variant {
        if (a.empty()) throwErr(450, errDesc(450));
        std::string p = toStr(a[0]);
        if (!p.empty() && p[0] == '/') {
            std::error_code ec;
            return Variant::mkStr(std::filesystem::path(ip.scriptFullName).parent_path().string() + p);
        }
        std::error_code ec;
        return Variant::mkStr((std::filesystem::path(ip.scriptFullName).parent_path() / p).string());
    };
    srv->methods["execute"] = [&ip](MapObject*, std::vector<Variant>& a) -> Variant {
        if (a.empty()) throwErr(450, errDesc(450));
        std::error_code ec;
        std::string path = (std::filesystem::path(ip.scriptFullName).parent_path() / toStr(a[0])).string();
        std::ifstream in(path, std::ios::binary);
        if (!in) throwErr(53, "File not found: " + path);
        std::ostringstream ss;
        ss << in.rdbuf();
        std::string code = ss.str();
        if (path.size() >= 4 && lower(path.substr(path.size() - 4)) == ".asp") code = aspToVbs(code);
        ip.executeString(code, true);
        return Variant();
    };
    srv->methods["transfer"] = srv->methods["execute"];

    // ---- Session / Application (SQLite-backed) ----
    std::string sid;
    {
        const char* hc = std::getenv("HTTP_COOKIE");
        auto m = parsePairs(hc ? hc : "");
        auto it = m.find("mscriptsessionid");
        if (it != m.end() && !it->second.empty()) sid = it->second;
    }
    auto timeoutMin = std::make_shared<long long>(20);
    if (sid.empty()) {
        sid = newSessionId(ip);
        if (g_resp) g_resp->cookies.push_back("MScriptSessionID=" + sid + "; path=/");
    }
    if (sessionExpired(sid, *timeoutMin)) stateRemoveAll(sid);
    sessionTouch(sid);
    auto session = makeStateColl(ip, true, sid, timeoutMin);
    session->methods["abandon"] = [sid](MapObject*, std::vector<Variant>&) -> Variant {
        stateRemoveAll(sid);
        return Variant();
    };
    session->props["timeout"] = Variant::mkLong(*timeoutMin);
    auto app = makeStateColl(ip, false, "", timeoutMin);
    app->methods["lock"] = [](MapObject*, std::vector<Variant>&) -> Variant { return Variant(); };
    app->methods["unlock"] = [](MapObject*, std::vector<Variant>&) -> Variant { return Variant(); };

    ip.global.vars["request"] = Variant::mkObj(req);
    ip.global.vars["response"] = Variant::mkObj(resp);
    ip.global.vars["server"] = Variant::mkObj(srv);
    ip.global.vars["session"] = Variant::mkObj(session);
    ip.global.vars["application"] = Variant::mkObj(app);
}

bool aspResponseWrite(Interp& ip, const std::string& s) {
    if (!ip.aspMode || !ip.responseObj) return false;
    responseBody(s);
    return true;
}

void flushAspResponse(Interp& ip) {
    (void)ip;
    if (!g_resp) return;
    // sync Response property writes (Buffer/ContentType/Status) into the state
    if (ip.responseObj) {
        auto* m = static_cast<MapObject*>(ip.responseObj.get());
        auto it = m->props.find("buffer");
        if (it != m->props.end()) g_resp->buffer = toBool(it->second);
        it = m->props.find("contenttype");
        if (it != m->props.end()) g_resp->contentType = toStr(it->second);
        it = m->props.find("status");
        if (it != m->props.end()) g_resp->status = toStr(it->second);
        it = m->props.find("expires");
        if (it != m->props.end() && it->second.t != VT::Empty) {
            time_t t = std::time(nullptr) + (time_t)(toInt64(it->second) * 60);
            char buf[64];
            std::strftime(buf, sizeof buf, "%a, %d %b %Y %H:%M:%S GMT", std::gmtime(&t));
            g_resp->headers.push_back({"Expires", buf});
        }
    }
    if (!g_resp->headersSent) {
        ensureHeaders();
        std::fwrite(g_resp->body.data(), 1, g_resp->body.size(), stdout);
        std::fflush(stdout);
        g_resp->body.clear();
    } else if (!g_resp->body.empty()) {
        std::fwrite(g_resp->body.data(), 1, g_resp->body.size(), stdout);
        std::fflush(stdout);
        g_resp->body.clear();
    }
}
