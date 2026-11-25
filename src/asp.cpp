// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 Lucas Holt. All rights reserved.

// asp.cpp — classic-ASP extension: .asp templates (mixed HTML / <% %> code),
// the CGI object model, and SQLite-backed Session/Application state.
// State lives in /var/db/mscript (override with MScript_ASP_STATE_DB).
#include "asp.h"

#include "interpreter.h"
#include "objects.h"
#include "parser.h"
#include "lexer.h"

#include <sqlite3.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <format>
#include <map>
#include <sstream>
#include <string>
#include <vector>
#include <fcntl.h>
#include <sys/stat.h>
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

// ---------- URL / form / cookie decoding ----------

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

// parse "k=v&k2=v2" (query string / urlencoded form). Repeated keys are joined
// with commas, as classic ASP does for Request.QueryString("k").
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
            auto it = m.find(k);
            if (it == m.end()) m[k] = v;
            else it->second += "," + v; // repeated field: comma-joined like ASP
        }
        if (amp == std::string::npos) break;
        pos = amp + 1;
    }
    return m;
}

// cookies use '; ' as the delimiter, not '&'
std::map<std::string, std::string> parseCookies(const std::string& s) {
    std::map<std::string, std::string> m;
    size_t pos = 0;
    while (pos <= s.size()) {
        size_t sep = s.find(';', pos);
        std::string pair = s.substr(pos, sep == std::string::npos ? std::string::npos : sep - pos);
        // trim leading spaces
        size_t b = pair.find_first_not_of(" \t");
        if (b != std::string::npos) pair = pair.substr(b);
        if (!pair.empty()) {
            size_t eq = pair.find('=');
            std::string k = lower(urlDecode(eq == std::string::npos ? pair : pair.substr(0, eq)));
            std::string v = eq == std::string::npos ? "" : urlDecode(pair.substr(eq + 1));
            m[k] = v;
        }
        if (sep == std::string::npos) break;
        pos = sep + 1;
    }
    return m;
}

// basic multipart/form-data parsing: returns field name -> value
std::map<std::string, std::string> parseMultipart(const std::string& body, const std::string& contentType) {
    std::map<std::string, std::string> m;
    size_t bpos = contentType.find("boundary=");
    if (bpos == std::string::npos) return m;
    std::string boundary = contentType.substr(bpos + 9);
    if (!boundary.empty() && boundary.front() == '"') boundary = boundary.substr(1, boundary.size() - 2);
    std::string delim = "--" + boundary;
    size_t pos = body.find(delim);
    while (pos != std::string::npos) {
        size_t start = pos + delim.size();
        if (body.compare(start, 2, "--") == 0) break; // final boundary
        size_t next = body.find(delim, start);
        if (next == std::string::npos) break;
        std::string part = body.substr(start, next - start);
        // headers end at the first blank line
        size_t hdrEnd = part.find("\r\n\r\n");
        if (hdrEnd == std::string::npos) hdrEnd = part.find("\n\n");
        if (hdrEnd == std::string::npos) { pos = next; continue; }
        std::string headers = part.substr(0, hdrEnd);
        std::string value = part.substr(hdrEnd + (part[hdrEnd] == '\r' ? 4 : 3));
        // strip trailing CRLF before the next boundary
        if (value.size() >= 2 && value.compare(value.size() - 2, 2, "\r\n") == 0) value.resize(value.size() - 2);
        else if (!value.empty() && value.back() == '\n') value.pop_back();
        size_t npos = headers.find("name=\"");
        if (npos != std::string::npos) {
            size_t nend = headers.find('"', npos + 6);
            if (nend != std::string::npos) {
                std::string name = lower(headers.substr(npos + 6, nend - (npos + 6)));
                auto it = m.find(name);
                if (it == m.end()) m[name] = value;
                else it->second += "," + value;
            }
        }
        pos = next;
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
MapObject* g_respObj = nullptr; // for property sync (Response.X = v)

// reject CR/LF in header names/values (response splitting)
std::string cleanHeaderText(const std::string& s, const char* what) {
    if (s.find('\r') != std::string::npos || s.find('\n') != std::string::npos)
        throwErr(5, std::string("ASP response: CR/LF not allowed in ") + what);
    return s;
}

// pull Response.X = v property writes into the live response state
void syncRespProps() {
    if (!g_resp || !g_respObj) return;
    auto it = g_respObj->props.find("buffer");
    if (it != g_respObj->props.end()) g_resp->buffer = toBool(it->second);
    it = g_respObj->props.find("contenttype");
    if (it != g_respObj->props.end()) g_resp->contentType = cleanHeaderText(toStr(it->second), "ContentType");
    it = g_respObj->props.find("status");
    if (it != g_respObj->props.end()) g_resp->status = cleanHeaderText(toStr(it->second), "Status");
    it = g_respObj->props.find("expires");
    if (it != g_respObj->props.end() && it->second.t != VT::Empty) {
        time_t t = std::time(nullptr) + (time_t)(toInt64(it->second) * 60);
        char buf[64];
        std::strftime(buf, sizeof buf, "%a, %d %b %Y %H:%M:%S GMT", std::gmtime(&t));
        g_resp->headers.push_back({"Expires", buf});
        g_respObj->props.erase(it);
    }
}

void ensureHeaders() {
    if (!g_resp || g_resp->headersSent) return;
    g_resp->headersSent = true;
    if (!std::getenv("REQUEST_METHOD")) return; // local run: body only
    syncRespProps();
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
    syncRespProps();
    if (g_resp->buffer) g_resp->body += s;
    else {
        ensureHeaders();
        std::fwrite(s.data(), 1, s.size(), stdout);
        std::fflush(stdout);
    }
}

// ---------- request body (read once, capped, shared by Form and BinaryRead) ----------

constexpr long long MAX_BODY = 16LL * 1024 * 1024; // 16 MiB cap
std::string g_reqBody;
size_t g_reqBodyPos = 0;
bool g_reqBodyRead = false;

const std::string& requestBody() {
    if (!g_reqBodyRead) {
        g_reqBodyRead = true;
        const char* cl = std::getenv("CONTENT_LENGTH");
        long long total = cl ? atoll(cl) : 0;
        if (total > MAX_BODY) total = MAX_BODY; // cap: oversized bodies are truncated
        if (total > 0) {
            g_reqBody.resize((size_t)total);
            size_t got = std::fread(g_reqBody.data(), 1, (size_t)total, stdin);
            g_reqBody.resize(got);
        }
    }
    return g_reqBody;
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

std::string g_appNs; // namespace for Application keys (per ASP "application" = script)

std::string stateDbPath() {
    const char* env = std::getenv("MScript_ASP_STATE_DB");
    if (env && *env) return env;
    return "/var/db/mscript/asp-state.db";
}

sqlite3* stateDb() {
    static sqlite3* db = []() -> sqlite3* {
        std::string path = stateDbPath();
        const char* env = std::getenv("MScript_ASP_STATE_DB");
        if (!env || !*env) {
            // default location: /var/db/mscript, owner-only
            std::error_code ec;
            std::filesystem::create_directories("/var/db/mscript", ec);
            std::filesystem::permissions("/var/db/mscript",
                                         std::filesystem::perms::owner_all,
                                         std::filesystem::perm_options::replace, ec);
        } else {
            std::error_code ec;
            std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
        }
        umask(0077); // state files must be owner-only
        sqlite3* d = nullptr;
        if (sqlite3_open(path.c_str(), &d) != SQLITE_OK) {
            if (d) sqlite3_close(d);
            // site-specific private fallback (never a shared /tmp file)
            std::string dir = "/tmp/mscript-asp-" + std::to_string((long long)getuid());
            std::error_code ec;
            std::filesystem::create_directories(dir, ec);
            std::filesystem::permissions(dir, std::filesystem::perms::owner_all,
                                         std::filesystem::perm_options::replace, ec);
            path = dir + "/asp-state.db";
            if (sqlite3_open(path.c_str(), &d) != SQLITE_OK) {
                if (d) sqlite3_close(d);
                return nullptr;
            }
        }
        chmod(path.c_str(), 0600);
        sqlite3_exec(d,
            "PRAGMA journal_mode=WAL;"
            "CREATE TABLE IF NOT EXISTS app(k TEXT PRIMARY KEY, v TEXT);"
            "CREATE TABLE IF NOT EXISTS sessions(sid TEXT, k TEXT, v TEXT, PRIMARY KEY(sid, k));"
            "CREATE TABLE IF NOT EXISTS session_meta(sid TEXT PRIMARY KEY, last REAL);",
            nullptr, nullptr, nullptr);
        return d;
    }();
    return db;
}

[[noreturn]] void stateFail(const char* what) {
    throw VBSError(1001, std::string("ASP state: ") + what + " failed");
}

// app rows are namespaced by the ASP application (script name) so independent
// sites sharing one database do not collide
std::string appKey(const std::string& k) { return g_appNs + "|" + k; }

std::string stateGet(bool isSession, const std::string& sid, const std::string& k) {
    sqlite3* db = stateDb();
    if (!db) return "";
    sqlite3_stmt* st = nullptr;
    const char* sql = isSession
        ? "SELECT v FROM sessions WHERE sid=?1 AND k=?2"
        : "SELECT v FROM app WHERE k=?1";
    if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) != SQLITE_OK) return "";
    if (isSession) {
        sqlite3_bind_text(st, 1, sid.data(), (int)sid.size(), SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, k.data(), (int)k.size(), SQLITE_TRANSIENT);
    } else {
        std::string ak = appKey(k);
        sqlite3_bind_text(st, 1, ak.data(), (int)ak.size(), SQLITE_TRANSIENT);
    }
    std::string out;
    if (sqlite3_step(st) == SQLITE_ROW) {
        const unsigned char* t = sqlite3_column_text(st, 0);
        if (t) out = (const char*)t;
    }
    sqlite3_finalize(st);
    return out;
}

void stateSet(bool isSession, const std::string& sid, const std::string& k, const std::string& v) {
    sqlite3* db = stateDb();
    if (!db) stateFail("database unavailable");
    sqlite3_stmt* st = nullptr;
    const char* sql = isSession
        ? "INSERT OR REPLACE INTO sessions(sid, k, v) VALUES(?1, ?2, ?3)"
        : "INSERT OR REPLACE INTO app(k, v) VALUES(?1, ?2)";
    if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) != SQLITE_OK) stateFail("prepare");
    if (isSession) {
        sqlite3_bind_text(st, 1, sid.data(), (int)sid.size(), SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, k.data(), (int)k.size(), SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 3, v.data(), (int)v.size(), SQLITE_TRANSIENT);
    } else {
        std::string ak = appKey(k);
        sqlite3_bind_text(st, 1, ak.data(), (int)ak.size(), SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, v.data(), (int)v.size(), SQLITE_TRANSIENT);
    }
    if (sqlite3_step(st) != SQLITE_DONE) stateFail("write");
    sqlite3_finalize(st);
}

void stateRemoveAll(const std::string& sid) {
    sqlite3* db = stateDb();
    if (!db) return;
    // prepared statements: the sid comes from a cookie and must never be
    // interpolated into SQL text
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db, "DELETE FROM sessions WHERE sid=?1", -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, sid.data(), (int)sid.size(), SQLITE_TRANSIENT);
        sqlite3_step(st);
        sqlite3_finalize(st);
    }
    if (sqlite3_prepare_v2(db, "DELETE FROM session_meta WHERE sid=?1", -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, sid.data(), (int)sid.size(), SQLITE_TRANSIENT);
        sqlite3_step(st);
        sqlite3_finalize(st);
    }
}

void sessionTouch(const std::string& sid) {
    sqlite3* db = stateDb();
    if (!db) return;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db, "INSERT OR REPLACE INTO session_meta(sid, last) VALUES(?1, ?2)", -1, &st, nullptr) != SQLITE_OK)
        return;
    sqlite3_bind_text(st, 1, sid.data(), (int)sid.size(), SQLITE_TRANSIENT);
    sqlite3_bind_double(st, 2, (double)std::time(nullptr));
    if (sqlite3_step(st) != SQLITE_DONE) stateFail("touch");
    sqlite3_finalize(st);
}

bool sessionExpired(const std::string& sid, long long timeoutMin) {
    sqlite3* db = stateDb();
    if (!db || timeoutMin <= 0) return false;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT last FROM session_meta WHERE sid=?1", -1, &st, nullptr) != SQLITE_OK)
        return false;
    sqlite3_bind_text(st, 1, sid.data(), (int)sid.size(), SQLITE_TRANSIENT);
    bool expired = true;
    if (sqlite3_step(st) == SQLITE_ROW) {
        double last = sqlite3_column_double(st, 0);
        expired = (std::time(nullptr) - (time_t)last) > timeoutMin * 60;
    }
    sqlite3_finalize(st);
    return expired;
}

// cross-process lock for Application.Lock/Unlock (flock on a file next to the db)
int appLockFd() {
    static int fd = []() -> int {
        std::string path = stateDbPath() + ".lock";
        int f = open(path.c_str(), O_RDWR | O_CREAT, 0600);
        if (f < 0) {
            std::string dir = "/tmp/mscript-asp-" + std::to_string((long long)getuid());
            std::error_code ec;
            std::filesystem::create_directories(dir, ec);
            f = open((dir + "/asp-state.lock").c_str(), O_RDWR | O_CREAT, 0600);
        }
        return f;
    }();
    return fd;
}

// ---------- JSON codec for persisted values (self-contained) ----------

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

std::string toJsonText(const Variant& v) {
    std::string out;
    serJson(out, v, 0);
    return out;
}

Variant fromJsonText(const std::string& s) {
    if (s.empty()) return Variant();
    try {
        return JsonParser(s).parse();
    } catch (VBSError&) {
        return Variant::mkStr(s); // not JSON: return raw string
    }
}

// cryptographic session ids from the OS CSPRNG
std::string newSessionId() {
    unsigned char buf[16];
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        size_t got = 0;
        while (got < sizeof buf) {
            ssize_t r = read(fd, buf + got, sizeof buf - got);
            if (r <= 0) break;
            got += (size_t)r;
        }
        close(fd);
        if (got == sizeof buf) {
            char hex[33];
            for (int i = 0; i < 16; i++) std::snprintf(hex + i * 2, 3, "%02x", buf[i]);
            return std::string(hex, 32);
        }
    }
    // last resort (should not happen on MidnightBSD): random + pid + time
    char fb[48];
    std::snprintf(fb, sizeof fb, "%lx%lx%lx", (unsigned long)std::rand(),
                  (unsigned long)getpid(), (unsigned long)std::time(nullptr));
    return lower(fb);
}

bool validSessionId(const std::string& s) {
    if (s.size() != 32) return false;
    for (char c : s)
        if (!std::isxdigit((unsigned char)c)) return false;
    return true;
}

std::string stateKeyOf(std::vector<Variant>& a) {
    if (a.empty()) throwErr(450, errDesc(450));
    return lower(toStr(a[0]));
}

// stateful collection object backed by the sqlite store
std::shared_ptr<MapObject> makeStateColl(Interp& ip, bool isSession, std::shared_ptr<std::string> sid) {
    auto c = std::make_shared<MapObject>();
    c->kind = isSession ? "Session" : "Application";
    if (isSession) c->props["sessionid"] = Variant::mkStr(*sid);
    auto timeoutOf = [c]() -> long long {
        auto it = c->props.find("timeout");
        return it == c->props.end() ? 20 : toInt64(it->second);
    };
    if (isSession) c->props["timeout"] = Variant::mkLong(20);
    c->onInvoke = [isSession, sid, timeoutOf](MapObject*, std::vector<Variant>& a) -> Variant {
        std::string k = stateKeyOf(a);
        if (isSession) {
            if (sessionExpired(*sid, timeoutOf())) { stateRemoveAll(*sid); return Variant(); }
            sessionTouch(*sid);
            return fromJsonText(stateGet(true, *sid, k));
        }
        return fromJsonText(stateGet(false, "", k));
    };
    c->onAssignInvoke = [isSession, sid](MapObject*, std::vector<Variant>& a, const Variant& v) {
        std::string k = stateKeyOf(a);
        if (isSession) sessionTouch(*sid);
        stateSet(isSession, isSession ? *sid : "", k, toJsonText(v));
    };
    if (isSession) {
        c->methods["abandon"] = [c, sid](MapObject*, std::vector<Variant>&) -> Variant {
            stateRemoveAll(*sid);
            // rotate: the abandoned id must not be reused
            *sid = newSessionId();
            c->props["sessionid"] = Variant::mkStr(*sid);
            if (g_resp) {
                for (auto& ck : g_resp->cookies)
                    if (ck.rfind("MScriptSessionID=", 0) == 0) ck = "MScriptSessionID=" + *sid + "; path=/";
                g_resp->cookies.push_back("MScriptSessionID=" + *sid + "; path=/");
            }
            return Variant();
        };
    } else {
        c->methods["lock"] = [](MapObject*, std::vector<Variant>&) -> Variant {
            int fd = appLockFd();
            if (fd >= 0) flock(fd, LOCK_EX); // cross-process exclusive lock
            return Variant();
        };
        c->methods["unlock"] = [](MapObject*, std::vector<Variant>&) -> Variant {
            int fd = appLockFd();
            if (fd >= 0) flock(fd, LOCK_UN);
            return Variant();
        };
    }
    return c;
}

} // namespace

void registerAspObjects(Interp& ip, bool cgi) {
    (void)cgi;
    // Application values are namespaced by the script (the ASP "application")
    {
        std::error_code ec;
        std::string fn = std::filesystem::path(ip.scriptFullName).filename().string();
        g_appNs = lower(fn.empty() ? "default" : fn);
    }

    // ---- Response ----
    g_resp = std::make_shared<AspResponse>();
    auto resp = std::make_shared<MapObject>();
    resp->kind = "Response";
    ip.responseObj = resp;
    g_respObj = resp.get();
    resp->props["buffer"] = Variant::mkBool(true);
    resp->props["contenttype"] = Variant::mkStr("text/html");
    resp->props["status"] = Variant::mkStr("200 OK");
    resp->methods["write"] = [](MapObject*, std::vector<Variant>& a) -> Variant {
        for (auto& v : a) responseBody(toStr(v));
        return Variant();
    };
    resp->methods["binarywrite"] = [](MapObject*, std::vector<Variant>& a) -> Variant {
        for (auto& v : a) {
            if (v.t == VT::Array && v.arr) {
                for (auto& e : v.arr->data) {
                    // each element is a raw byte, not decimal text
                    responseBody(std::string(1, (char)(unsigned char)toInt64(e)));
                }
            } else {
                responseBody(toStr(v));
            }
        }
        return Variant();
    };
    resp->methods["flush"] = [](MapObject*, std::vector<Variant>&) -> Variant {
        if (g_resp) {
            syncRespProps();
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
            g_resp->headers.push_back({"Location", cleanHeaderText(toStr(a[0]), "redirect URL")});
            g_resp->ended = true;
        }
        throw EndSig{};
    };
    resp->methods["addheader"] = [](MapObject*, std::vector<Variant>& a) -> Variant {
        if (a.size() < 2) throwErr(450, errDesc(450));
        if (g_resp)
            g_resp->headers.push_back({cleanHeaderText(toStr(a[0]), "header name"),
                                       cleanHeaderText(toStr(a[1]), "header value")});
        return Variant();
    };

    // Response.Cookies("name") = value / read
    auto cookies = std::make_shared<MapObject>();
    cookies->kind = "Cookies";
    auto reqCookies = parseCookies(std::getenv("HTTP_COOKIE") ? std::getenv("HTTP_COOKIE") : "");
    cookies->onAssignInvoke = [](MapObject*, std::vector<Variant>& a, const Variant& v) {
        if (a.empty() || !g_resp) return;
        g_resp->cookies.push_back(cleanHeaderText(toStr(a[0]), "cookie name") + "=" +
                                  urlEncode(toStr(v)) + "; path=/");
    };
    cookies->onInvoke = [reqCookies](MapObject*, std::vector<Variant>& a) -> Variant {
        if (a.empty()) throwErr(450, errDesc(450));
        auto it = reqCookies.find(lower(toStr(a[0])));
        return Variant::mkStr(it == reqCookies.end() ? "" : it->second);
    };
    resp->props["cookies"] = Variant::mkObj(cookies);

    // ---- Request ----
    auto req = std::make_shared<MapObject>();
    req->kind = "Request";
    req->props["querystring"] = Variant::mkObj(makeColl(
        parsePairs(std::getenv("QUERY_STRING") ? std::getenv("QUERY_STRING") : ""), "QueryString"));
    std::map<std::string, std::string> form;
    {
        const char* rm = std::getenv("REQUEST_METHOD");
        const char* ct = std::getenv("CONTENT_TYPE");
        if (rm && !strcmp(rm, "POST") && ct) {
            const std::string& body = requestBody(); // read once, capped
            if (strncmp(ct, "application/x-www-form-urlencoded", 33) == 0)
                form = parsePairs(body);
            else if (strncmp(ct, "multipart/form-data", 19) == 0)
                form = parseMultipart(body, ct);
        }
    }
    req->props["form"] = Variant::mkObj(makeColl(form, "Form"));
    req->props["cookies"] = Variant::mkObj(makeColl(reqCookies, "Cookies"));
    auto sv = std::make_shared<MapObject>();
    sv->kind = "ServerVariables";
    sv->onInvoke = [](MapObject*, std::vector<Variant>& a) -> Variant {
        if (a.empty()) throwErr(450, errDesc(450));
        const char* v = std::getenv(toStr(a[0]).c_str());
        return Variant::mkStr(v ? v : "");
    };
    req->props["servervariables"] = Variant::mkObj(sv);
    req->props["totalbytes"] = Variant::mkLong([] {
        const char* cl = std::getenv("CONTENT_LENGTH");
        long long n = cl ? atoll(cl) : 0;
        return n > MAX_BODY ? MAX_BODY : n;
    }());
    req->methods["binaryread"] = [](MapObject*, std::vector<Variant>& a) -> Variant {
        long long len = a.empty() ? -1 : toInt64(a[0]);
        const std::string& body = requestBody();
        if (len < 0 || (size_t)len > body.size() - g_reqBodyPos) len = (long long)(body.size() - g_reqBodyPos);
        if (len > MAX_BODY) len = MAX_BODY;
        if (len <= 0) return Variant::mkArr(std::make_shared<VArray>());
        auto arr = std::make_shared<VArray>();
        arr->alloc({len}, 0, false);
        for (long long i = 0; i < len; i++)
            arr->data[(size_t)i] = Variant::mkByte((unsigned char)body[g_reqBodyPos + (size_t)i]);
        g_reqBodyPos += (size_t)len;
        return Variant::mkArr(arr);
    };

    // ---- Server ----
    auto srv = std::make_shared<MapObject>();
    srv->kind = "Server";
    srv->props["scripttimeout"] = Variant::mkLong(90);
    ip.serverObj = srv;
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
        std::error_code ec;
        std::filesystem::path base = std::filesystem::path(ip.scriptFullName).parent_path();
        std::string p = toStr(a[0]);
        std::filesystem::path full = (p.empty() || p[0] == '/') ? base / p.substr(p[0] == '/' ? 1 : 0) : base / p;
        return Variant::mkStr(full.lexically_normal().string());
    };
    // resolve and confine to the script's directory (no traversal, no absolute escape)
    auto resolveConfined = [&ip](const std::string& p) -> std::string {
        std::error_code ec;
        std::filesystem::path base = std::filesystem::absolute(ip.scriptFullName, ec).parent_path();
        std::filesystem::path full = (p.empty() || p[0] == '/') ? base / p.substr(p[0] == '/' ? 1 : 0) : base / p;
        full = full.lexically_normal();
        std::string bs = base.lexically_normal().string();
        std::string fs = full.string();
        if (fs != bs && fs.rfind(bs + "/", 0) != 0)
            throwErr(5, "ASP Server.Execute/Transfer: path outside the script directory");
        return fs;
    };
    auto doExecute = [&ip, resolveConfined](const std::string& rel) {
        std::string path = resolveConfined(rel);
        std::ifstream in(path, std::ios::binary);
        if (!in) throwErr(53, "File not found: " + path);
        std::ostringstream ss;
        ss << in.rdbuf();
        std::string code = ss.str();
        if (path.size() >= 4 && lower(path.substr(path.size() - 4)) == ".asp") code = aspToVbs(code);
        ip.executeString(code, true);
    };
    srv->methods["execute"] = [doExecute](MapObject*, std::vector<Variant>& a) -> Variant {
        if (a.empty()) throwErr(450, errDesc(450));
        doExecute(toStr(a[0]));
        return Variant();
    };
    srv->methods["transfer"] = [doExecute](MapObject*, std::vector<Variant>& a) -> Variant {
        if (a.empty()) throwErr(450, errDesc(450));
        doExecute(toStr(a[0]));
        throw EndSig{}; // Transfer does not return to the original page
    };

    // ---- Session / Application (SQLite-backed) ----
    auto sid = std::make_shared<std::string>();
    {
        auto it = reqCookies.find("mscriptsessionid");
        // only accept well-formed ids; a client-chosen garbage id is replaced
        if (it != reqCookies.end() && validSessionId(it->second)) *sid = it->second;
    }
    if (sid->empty()) {
        *sid = newSessionId();
        if (g_resp) g_resp->cookies.push_back("MScriptSessionID=" + *sid + "; path=/");
    }
    if (sessionExpired(*sid, 20)) stateRemoveAll(*sid);
    sessionTouch(*sid);
    auto session = makeStateColl(ip, true, sid);
    auto app = makeStateColl(ip, false, sid);

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
    syncRespProps();
    if (!g_resp->headersSent) ensureHeaders();
    if (!g_resp->body.empty()) {
        std::fwrite(g_resp->body.data(), 1, g_resp->body.size(), stdout);
        std::fflush(stdout);
        g_resp->body.clear();
    }
}
