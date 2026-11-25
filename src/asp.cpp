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
#include <cerrno>
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
#include <optional>
#include <functional>
#include <set>
#include <sstream>
#include <string>
#include <vector>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

extern char** environ; // file scope: inside the anonymous namespace it would get internal linkage

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

std::string aspToVbs(const std::string& src, bool dropText) {
    std::string out;
    out.reserve(src.size() + src.size() / 3 + 16);
    std::string lit;
    bool skipNl = false; // a <%@ %> directive also swallows the following newline
    // dropText: only code blocks are kept (global.asa — its HTML is never rendered)
    auto emitLit = [&](std::string& o, const std::string& l) { if (!dropText) emitLiteral(o, l); };
    size_t i = 0, n = src.size();
    while (i < n) {
        // <script runat="server"> ... </script> blocks (global.asa style)
        if (i + 7 <= n && lower(src.substr(i, 7)) == "<script") {
            size_t tagEnd = src.find('>', i + 7);
            if (tagEnd != std::string::npos) {
                std::string tag = lower(src.substr(i, tagEnd - i));
                if (tag.find("runat") != std::string::npos &&
                    tag.find("server") != std::string::npos) {
                    emitLit(out, lit);
                    lit.clear();
                    size_t close = std::string::npos;
                    for (size_t j = tagEnd + 1; j + 8 <= n; j++)
                        if (lower(src.substr(j, 8)) == "</script") { close = j; break; }
                    if (close == std::string::npos) close = n;
                    std::string code = src.substr(tagEnd + 1, close - (tagEnd + 1));
                    out += code;
                    if (!code.empty() && code.back() != '\n') out += '\n';
                    i = (close >= n) ? n : close + 9; // skip "</script>" incl. '>'
                    continue;
                }
            }
        }
        if (src.compare(i, 2, "<%") == 0) {
            if (skipNl) {
                // swallow the newline(s) immediately after the directive line
                while (!lit.empty() && (lit.front() == '\n' || lit.front() == '\r'))
                    lit.erase(lit.begin());
            }
            skipNl = false;
            emitLit(out, lit);
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
    emitLit(out, lit);
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
// Request collections hold at most this many fields (like PHP's max_input_vars
// or IIS's request limits): a 16 MiB body of k=v&k=v... otherwise cost
// hundreds of MB of map nodes and seconds of CPU before the page even ran
constexpr size_t MAX_FIELDS = 10000;

[[noreturn]] void tooManyFields() {
    throwErr(5, "ASP Request: too many form/query fields (limit " + std::to_string(MAX_FIELDS) + ")");
}

std::map<std::string, std::string> parsePairs(const std::string& s) {
    std::map<std::string, std::string> m;
    size_t pos = 0, fields = 0;
    while (pos <= s.size()) {
        size_t amp = s.find('&', pos);
        std::string pair = s.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos);
        if (!pair.empty()) {
            if (++fields > MAX_FIELDS) tooManyFields();
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

// cookies use '; ' as the delimiter, not '&'. Values are kept RAW (still
// URL-encoded): a cookie written with sub-keys (n=k=v&k2=v2) must be split
// on '&'/'=' before decoding, or an encoded '&' inside a sub-key value would
// split it. Use urlDecode() for the whole value.
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
            std::string v = eq == std::string::npos ? "" : pair.substr(eq + 1);
            // first occurrence wins: browsers send the most specific
            // (longest path) cookie first, so a cookie tossed in from a
            // broader path or a parent domain cannot override it
            m.emplace(k, v);
        }
        if (sep == std::string::npos) break;
        pos = sep + 1;
    }
    return m;
}

// basic multipart/form-data parsing: returns field name -> value
// the name="..." parameter of a part's Content-Disposition header. Parsed as
// parameters, so filename="x" (which contains name=") is never mistaken for
// the field name, and header/parameter names are case-insensitive.
bool multipartFieldName(const std::string& headers, std::string& name) {
    size_t pos = 0;
    while (pos < headers.size()) {
        size_t eol = headers.find('\n', pos);
        std::string line = headers.substr(pos, eol == std::string::npos ? std::string::npos : eol - pos);
        pos = eol == std::string::npos ? headers.size() : eol + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t colon = line.find(':');
        if (colon == std::string::npos || lower(trim(line.substr(0, colon))) != "content-disposition") continue;
        std::string rest = line.substr(colon + 1);
        size_t i = 0;
        while (i < rest.size()) {
            size_t semi = i;
            bool inq = false;
            while (semi < rest.size() && (inq || rest[semi] != ';')) {
                if (rest[semi] == '"') inq = !inq;
                semi++;
            }
            std::string param = trim(rest.substr(i, semi - i));
            i = semi + 1;
            size_t eq = param.find('=');
            if (eq == std::string::npos || lower(trim(param.substr(0, eq))) != "name") continue;
            std::string v = trim(param.substr(eq + 1));
            if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
            name = v;
            return true;
        }
    }
    return false;
}

std::map<std::string, std::string> parseMultipart(const std::string& body, const std::string& contentType) {
    std::map<std::string, std::string> m;
    size_t bpos = lower(contentType).find("boundary=");
    if (bpos == std::string::npos) return m;
    std::string boundary = contentType.substr(bpos + 9);
    if (!boundary.empty() && boundary.front() == '"') {
        size_t q = boundary.find('"', 1);
        boundary = boundary.substr(1, q == std::string::npos ? std::string::npos : q - 1);
    } else {
        boundary = boundary.substr(0, boundary.find(';')); // drop trailing parameters
        while (!boundary.empty() && (boundary.back() == ' ' || boundary.back() == '\t')) boundary.pop_back();
    }
    // RFC 2046: 1..70 characters. A huge boundary made every delimiter search
    // O(body * boundary)
    if (boundary.empty() || boundary.size() > 70) return m;
    size_t parts = 0;
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
        size_t sepLen = 4;
        if (hdrEnd == std::string::npos) { hdrEnd = part.find("\n\n"); sepLen = 2; }
        if (hdrEnd == std::string::npos) { pos = next; continue; }
        std::string headers = part.substr(0, hdrEnd);
        std::string value = part.substr(std::min(hdrEnd + sepLen, part.size()));
        // strip trailing CRLF before the next boundary
        if (value.size() >= 2 && value.compare(value.size() - 2, 2, "\r\n") == 0) value.resize(value.size() - 2);
        else if (!value.empty() && value.back() == '\n') value.pop_back();
        std::string name;
        if (multipartFieldName(headers, name)) {
            if (++parts > MAX_FIELDS) tooManyFields();
            name = lower(name);
            auto it = m.find(name);
            if (it == m.end()) m[name] = value;
            else it->second += "," + value;
        }
        pos = next;
    }
    return m;
}

// ---------- Response state ----------

// Response.Cookies(name), with classic-ASP semantics: the cookie's content is
// either a scalar value (Response.Cookies("n") = v) or a sub-key dictionary
// (Response.Cookies("n")("k") = v, sent as k=v&k2=v2). Attributes are set only
// through properties (.Path .Domain .Expires .Secure .HttpOnly .SameSite), so
// a sub-key name can never turn into a cookie attribute. Any write marks the
// cookie dirty so it gets a Set-Cookie header.
struct CookieObj : MapObject {
    std::string key;   // lowercased name
    std::string value; // scalar value (when there are no sub-keys)
    std::vector<std::pair<std::string, std::string>> subkeys; // original case, insertion order
    explicit CookieObj(std::string k);
    void markDirty();
    std::string content() const;
    Variant getMember(const std::string& n) override;
    void setMember(const std::string& n, const Variant& v) override;
    bool hasMember(const std::string& n) const override;
};

// one piece of the buffered body: text is transcoded per Session.CodePage at
// output time, binary (Response.BinaryWrite) is emitted byte-for-byte
struct BodyChunk {
    std::string data;
    bool binary;
};

struct AspResponse {
    std::vector<BodyChunk> body;
    std::string status = "200 OK";
    std::string contentType = "text/html";
    std::string charset;                                        // appended to Content-Type
    std::string cacheControl;                                   // Cache-Control header
    std::string pics;                                           // PICS-Label header
    std::string expiresAbs;                                     // Expires header (absolute)
    std::vector<std::pair<std::string, std::string>> headers; // extra headers
    std::map<std::string, std::shared_ptr<CookieObj>> outCookies; // Response.Cookies(name), original case
    std::set<std::string> dirtyCookies;                  // lowercased names actually written
    int outCp = 0;                                        // 1252/28591 -> transcode output to Latin-1
    bool expiresConsumed = false;                         // Response.Expires consumed once
    bool buffer = true;
    bool headersSent = false;
    bool ended = false;
};

// format a VB date serial as an HTTP date (RFC 1123)
std::string httpDate(double serial) {
    double secs = (serial - 25569.0) * 86400.0; // 1899-12-30 -> unix epoch
    // clamp to years 1..9999 before the time_t cast (an out-of-range double
    // cast is UB, and gmtime returns NULL for years past INT_MAX)
    constexpr double MIN_SECS = -62135596800.0; // 0001-01-01T00:00:00Z
    constexpr double MAX_SECS = 253402300799.0; // 9999-12-31T23:59:59Z
    if (!std::isfinite(secs) || secs < MIN_SECS) secs = MIN_SECS;
    if (secs > MAX_SECS) secs = MAX_SECS;
    time_t t = (time_t)secs;
    struct tm tmv;
    if (!gmtime_r(&t, &tmv)) return "Thu, 01 Jan 1970 00:00:00 GMT";
    char buf[64];
    if (std::strftime(buf, sizeof buf, "%a, %d %b %Y %H:%M:%S GMT", &tmv) == 0)
        return "Thu, 01 Jan 1970 00:00:00 GMT";
    return buf;
}

// one interpreter per CGI process
std::shared_ptr<AspResponse> g_resp;
MapObject* g_respObj = nullptr;   // for property sync (Response.X = v)
bool g_suppressOutput = false;    // Session_OnEnd running: it has no Response (as in IIS)
struct SessionObj;
long long g_sessionCodePage = 1252; // Session.CodePage -> output charset

// reject CR/LF in header names/values (response splitting)
std::string cleanHeaderText(const std::string& s, const char* what) {
    if (s.find('\r') != std::string::npos || s.find('\n') != std::string::npos ||
        s.find('\0') != std::string::npos)
        throwErr(5, std::string("ASP response: CR/LF/NUL not allowed in ") + what);
    return s;
}

// header changes after the headers went out cannot take effect: raise the
// IIS error instead of silently dropping them (ASP 0156, E_FAIL)
void requireHeadersUnsent() {
    if (g_resp && g_resp->headersSent)
        throw VBSError((int)0x80004005,
                       "Response object error 'ASP 0156 : 80004005': Header Error. "
                       "The HTTP headers are already written to the client browser.");
}

std::string htmlEncode(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            default: out += c;
        }
    }
    return out;
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
    // properties stay readable after syncing (only copied into state, never
    // erased); Response.Expires is relative to the first sync, so consume once
    it = g_respObj->props.find("expires");
    if (it != g_respObj->props.end() && it->second.t != VT::Empty && !g_resp->expiresConsumed) {
        g_resp->expiresConsumed = true;
        // minutes from now; computed in double so a huge value cannot
        // overflow (httpDate clamps the result)
        double mins = (double)toInt64(it->second);
        g_resp->expiresAbs = httpDate((double)std::time(nullptr) / 86400.0 + mins / 1440.0 + 25569.0);
    }
    auto take = [&](const char* prop, std::string& field, const char* what) {
        auto i2 = g_respObj->props.find(prop);
        if (i2 != g_respObj->props.end() && i2->second.t != VT::Empty)
            field = cleanHeaderText(toStr(i2->second), what);
    };
    take("cachecontrol", g_resp->cacheControl, "CacheControl");
    take("pics", g_resp->pics, "PICS");
    take("charset", g_resp->charset, "Charset");
    auto eit = g_respObj->props.find("expiresabsolute");
    if (eit != g_respObj->props.end() && eit->second.t != VT::Empty) {
        if (eit->second.t == VT::Date) g_resp->expiresAbs = httpDate(eit->second.d);
        else g_resp->expiresAbs = cleanHeaderText(toStr(eit->second), "ExpiresAbsolute");
    }
}

// cookie attribute values must not carry cookie delimiters (';' would inject
// extra directives) or CR/LF
std::string cleanCookieAttr(const std::string& s, const char* what) {
    if (s.find(';') != std::string::npos || s.find('\r') != std::string::npos ||
        s.find('\n') != std::string::npos || s.find('\0') != std::string::npos)
        throwErr(5, std::string("ASP cookie: ';', CR/LF and NUL not allowed in ") + what);
    return s;
}

// cookie names must be RFC 6265 tokens: a ';', '=', ',' or space in a name
// would inject attributes or split the header
std::string cleanCookieName(const std::string& s) {
    if (s.empty()) throwErr(5, "ASP cookie: empty cookie name");
    for (unsigned char c : s) {
        if (c <= 0x20 || c >= 0x7F || std::strchr("()<>@,;:\\\"/[]?={}", c))
            throwErr(5, "ASP cookie: invalid character in cookie name");
    }
    return s;
}

const std::set<std::string>& cookieAttrNames() {
    static const std::set<std::string> a = {"path", "domain", "expires", "secure", "httponly", "samesite"};
    return a;
}

CookieObj::CookieObj(std::string k) : key(std::move(k)) {
    kind = "Cookie";
    // sub-key dictionary: cookie("k") reads, cookie("k") = v writes
    onInvoke = [](MapObject* self, std::vector<Variant>& a) -> Variant {
        auto* ck = static_cast<CookieObj*>(self);
        if (a.empty()) return Variant::mkStr(ck->content());
        std::string k = lower(toStr(a[0]));
        for (auto& [sk, sv] : ck->subkeys)
            if (lower(sk) == k) return Variant::mkStr(sv);
        return Variant::mkStr("");
    };
    onAssignInvoke = [](MapObject* self, std::vector<Variant>& a, const Variant& v) {
        auto* ck = static_cast<CookieObj*>(self);
        if (a.empty()) throwErr(450, errDesc(450));
        std::string name = toStr(a[0]);
        std::string k = lower(name);
        std::string val = toStr(v);
        ck->value.clear(); // a cookie with keys has no scalar value
        bool found = false;
        for (auto& [sk, sv] : ck->subkeys)
            if (lower(sk) == k) { sv = val; found = true; break; }
        if (!found) ck->subkeys.emplace_back(name, val);
        ck->markDirty();
    };
}

void CookieObj::markDirty() {
    requireHeadersUnsent();
    if (g_resp) g_resp->dirtyCookies.insert(key);
}

std::string CookieObj::content() const {
    if (subkeys.empty()) return urlEncode(value);
    std::string out;
    for (auto& [k, v] : subkeys) {
        if (!out.empty()) out += '&';
        out += urlEncode(k) + "=" + urlEncode(v);
    }
    return out;
}

bool CookieObj::hasMember(const std::string& n) const {
    return n == "haskeys" || cookieAttrNames().count(n) || MapObject::hasMember(n);
}

Variant CookieObj::getMember(const std::string& n) {
    if (n == "haskeys") return Variant::mkBool(!subkeys.empty());
    if (cookieAttrNames().count(n)) {
        auto it = props.find(n);
        if (it != props.end()) return it->second;
        if (n == "secure" || n == "httponly") return Variant::mkBool(false);
        if (n == "expires") return Variant();
        return Variant::mkStr("");
    }
    return MapObject::getMember(n);
}

void CookieObj::setMember(const std::string& n, const Variant& v) {
    if (n == "haskeys") throwErr(438, errDesc(438)); // read-only
    if (!cookieAttrNames().count(n)) throwErr(438, errDesc(438));
    // validate text attributes now, at the assignment (a bad value must
    // never reach header generation, where it would abort the header block)
    if ((n == "path" || n == "domain" || n == "samesite" || n == "expires") && v.t != VT::Date)
        cleanCookieAttr(toStr(v), n.c_str());
    MapObject::setMember(n, v);
    markDirty(); // e.g. .Expires = <past date> alone deletes a cookie
}

// transcode UTF-8 output to Latin-1 when the session code page asks for it
std::string encodeOut(const std::string& s) {
    if (!g_resp || (g_resp->outCp != 1252 && g_resp->outCp != 28591)) return s;
    // strict UTF-8 decoding: overlong forms, surrogates, out-of-range code
    // points and non-continuation trail bytes are invalid. An invalid sequence
    // becomes '?' and only its lead byte is consumed, so a crafted lead byte
    // can never decode to markup ('<', '"', ...) or swallow the next character
    // (both would bypass Server.HTMLEncode)
    std::string out;
    out.reserve(s.size());
    const size_t n = s.size();
    auto cont = [&](size_t j) { return j < n && ((unsigned char)s[j] & 0xC0) == 0x80; };
    for (size_t i = 0; i < n;) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x80) { out += (char)c; i++; continue; }
        unsigned cp = 0; size_t len = 0; unsigned minCp = 0;
        if (c >= 0xC2 && c <= 0xDF) { len = 2; cp = c & 0x1F; minCp = 0x80; }
        else if (c >= 0xE0 && c <= 0xEF) { len = 3; cp = c & 0x0F; minCp = 0x800; }
        else if (c >= 0xF0 && c <= 0xF4) { len = 4; cp = c & 0x07; minCp = 0x10000; }
        bool ok = len != 0;
        for (size_t k = 1; ok && k < len; k++) {
            if (!cont(i + k)) ok = false;
            else cp = (cp << 6) | ((unsigned char)s[i + k] & 0x3F);
        }
        if (ok && (cp < minCp || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))) ok = false;
        if (!ok) { out += '?'; i++; continue; }
        out += (cp <= 0xFF) ? (char)cp : '?';
        i += len;
    }
    return out;
}

void ensureHeaders() {
    if (!g_resp || g_resp->headersSent) return;
    if (!std::getenv("REQUEST_METHOD")) { g_resp->headersSent = true; return; } // local run: body only
    // headersSent is set only once the whole header block has been built: if
    // building it throws, nothing (in particular no body) has been written,
    // and the next attempt or the final flush still emits headers. Setting
    // it first let the body be written as the header block.
    syncRespProps();
    // Session.CodePage drives the output charset when the page did not set one
    if (g_resp->charset.empty()) {
        switch (g_sessionCodePage) {
            case 65001: g_resp->charset = "utf-8"; break;
            case 1252: g_resp->charset = "windows-1252"; g_resp->outCp = 1252; break;
            case 28591: g_resp->charset = "iso-8859-1"; g_resp->outCp = 28591; break;
            case 1200: g_resp->charset = "utf-16"; break;
            default: break;
        }
    }
    std::string h;
    if (g_resp->status != "200 OK") h += "Status: " + g_resp->status + "\r\n";
    h += "Content-Type: " + g_resp->contentType +
         (g_resp->charset.empty() ? "" : "; charset=" + g_resp->charset) + "\r\n";
    if (!g_resp->cacheControl.empty()) h += "Cache-Control: " + g_resp->cacheControl + "\r\n";
    if (!g_resp->pics.empty()) h += "PICS-Label: " + g_resp->pics + "\r\n";
    if (!g_resp->expiresAbs.empty()) h += "Expires: " + g_resp->expiresAbs + "\r\n";
    for (auto& [name, ck] : g_resp->outCookies) {
        // only cookies the page actually wrote get a Set-Cookie header; a bare
        // read of Response.Cookies(name) must not emit an empty cookie
        if (!g_resp->dirtyCookies.count(lower(name))) continue;
        std::string c = cleanCookieName(name) + "=" + ck->content();
        auto attr = [&](const char* k) -> std::string {
            auto i2 = ck->props.find(k);
            return i2 == ck->props.end() ? "" : toStr(i2->second);
        };
        std::string path = attr("path");
        if (!path.empty()) c += "; path=" + cleanCookieAttr(path, "path");
        auto eit = ck->props.find("expires");
        if (eit != ck->props.end() && eit->second.t == VT::Date)
            c += "; expires=" + httpDate(eit->second.d);
        else {
            std::string ex = attr("expires");
            if (!ex.empty()) c += "; expires=" + cleanCookieAttr(ex, "expires");
        }
        std::string dom = attr("domain");
        if (!dom.empty()) c += "; domain=" + cleanCookieAttr(dom, "domain");
        auto sit = ck->props.find("secure");
        if (sit != ck->props.end() && toBool(sit->second)) c += "; secure";
        auto hit = ck->props.find("httponly");
        if (hit != ck->props.end() && toBool(hit->second)) c += "; httponly";
        std::string ss = attr("samesite");
        if (!ss.empty()) c += "; samesite=" + cleanCookieAttr(ss, "samesite");
        h += "Set-Cookie: " + c + "\r\n";
    }
    for (auto& kv : g_resp->headers) h += kv.first + ": " + kv.second + "\r\n";
    h += "\r\n";
    g_resp->headersSent = true;
    std::fwrite(h.data(), 1, h.size(), stdout);
}

// the single output path for body bytes (buffered flush, unbuffered writes
// and the final flush): text is transcoded, binary is written untouched
void emitChunk(const BodyChunk& c) {
    if (c.binary) {
        std::fwrite(c.data.data(), 1, c.data.size(), stdout);
    } else {
        std::string out = encodeOut(c.data);
        std::fwrite(out.data(), 1, out.size(), stdout);
    }
}

void flushBody() {
    if (!g_resp) return;
    ensureHeaders();
    for (auto& c : g_resp->body) emitChunk(c);
    g_resp->body.clear();
    std::fflush(stdout);
}

void responseBody(const std::string& s, bool binary = false) {
    if (!g_resp || g_resp->ended || g_suppressOutput) return;
    syncRespProps();
    if (g_resp->buffer) {
        if (!g_resp->body.empty() && g_resp->body.back().binary == binary)
            g_resp->body.back().data += s;
        else
            g_resp->body.push_back({s, binary});
    } else {
        ensureHeaders();
        emitChunk({s, binary});
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

std::string g_appRoot; // the application root dir (original case), for APPL_PHYSICAL_PATH
// request headers held back from the environment by cgiSanitizeEnv
std::map<std::string, std::string> g_heldHeaders;

// ---------- Request.Cookies ----------

// Request.Cookies("n"), as classic ASP's read cookie: the decoded value is
// the default property (so it still reads as a string), and a cookie written
// with sub-keys exposes them: Request.Cookies("n")("k"), .HasKeys, .Count.
struct ReqCookieObj : MapObject {
    std::string raw;                         // as received, URL-encoded
    std::map<std::string, std::string> keys; // decoded sub-keys, lowercased names
    bool hasKeys = false;
    explicit ReqCookieObj(std::string r);
    bool hasDefault() const override { return true; }
    Variant getDefault() override { return Variant::mkStr(urlDecode(raw)); }
};

ReqCookieObj::ReqCookieObj(std::string r) : raw(std::move(r)) {
    kind = "IReadCookie";
    // sub-keys exist only when the raw value has an unencoded '=' (a scalar
    // value written by Response.Cookies is URL-encoded, so its '=' is %3D)
    hasKeys = raw.find('=') != std::string::npos;
    if (hasKeys) keys = parsePairs(raw);
    props["haskeys"] = Variant::mkBool(hasKeys);
    props["count"] = Variant::mkLong(hasKeys ? (long long)keys.size() : 0);
    onInvoke = [](MapObject* self, std::vector<Variant>& a) -> Variant {
        auto* ck = static_cast<ReqCookieObj*>(self);
        if (a.empty()) return ck->getDefault();
        auto it = ck->keys.find(lower(toStr(a[0])));
        return Variant::mkStr(it == ck->keys.end() ? "" : it->second);
    };
    methods["item"] = onInvoke;
    onEnum = [](MapObject* self) -> std::vector<Variant> {
        std::vector<Variant> out;
        for (auto& kv : static_cast<ReqCookieObj*>(self)->keys) out.push_back(Variant::mkStr(kv.first));
        return out;
    };
}

std::shared_ptr<MapObject> makeReqCookies(const std::map<std::string, std::string>& raw) {
    auto c = std::make_shared<MapObject>();
    c->kind = "Cookies";
    c->props["count"] = Variant::mkLong((long long)raw.size());
    // a missing cookie is an empty cookie object (reads as ""), as in ASP
    c->onInvoke = [raw](MapObject*, std::vector<Variant>& a) -> Variant {
        if (a.empty()) throwErr(450, errDesc(450));
        auto it = raw.find(lower(toStr(a[0])));
        return Variant::mkObj(std::make_shared<ReqCookieObj>(it == raw.end() ? "" : it->second));
    };
    c->methods["item"] = c->onInvoke;
    c->onEnum = [raw](MapObject*) -> std::vector<Variant> {
        std::vector<Variant> out;
        for (auto& kv : raw) out.push_back(Variant::mkStr(kv.first));
        return out;
    };
    return c;
}

// ---------- Request.ServerVariables ----------
//
// Only request metadata is exposed, not the whole process environment: the
// RFC 3875 CGI variables, the request headers (HTTP_*), TLS details (SSL_*,
// HTTPS) and common web-server extras, plus the IIS names synthesized from
// them. Anything else in the environment (secrets passed with SetEnv, PATH,
// MScript_ASP_STATE_DB, ...) is not reachable through ServerVariables or
// the Request("name") fallback.

const std::set<std::string>& cgiVarNames() {
    static const std::set<std::string> n = {
        // RFC 3875
        "AUTH_TYPE", "CONTENT_LENGTH", "CONTENT_TYPE", "GATEWAY_INTERFACE", "PATH_INFO",
        "PATH_TRANSLATED", "QUERY_STRING", "REMOTE_ADDR", "REMOTE_HOST", "REMOTE_IDENT",
        "REMOTE_USER", "REQUEST_METHOD", "SCRIPT_NAME", "SERVER_NAME", "SERVER_PORT",
        "SERVER_PROTOCOL", "SERVER_SOFTWARE",
        // common server extras (Apache, nginx, lighttpd)
        "DOCUMENT_ROOT", "HTTPS", "REDIRECT_STATUS", "REDIRECT_URL", "REMOTE_PORT",
        "REQUEST_SCHEME", "REQUEST_URI", "SCRIPT_FILENAME", "SCRIPT_URI", "SCRIPT_URL",
        "SERVER_ADDR", "SERVER_ADMIN", "SERVER_SIGNATURE", "CONTEXT_DOCUMENT_ROOT",
        "CONTEXT_PREFIX", "UNIQUE_ID",
    };
    return n;
}

bool exposedEnvName(const std::string& up) {
    return cgiVarNames().count(up) || up.rfind("HTTP_", 0) == 0 || up.rfind("SSL_", 0) == 0;
}

std::string envOr(const char* name, const char* dflt = "") {
    const char* v = std::getenv(name);
    return v ? v : dflt;
}

// HTTP_ACCEPT_LANGUAGE -> Accept-Language (for ALL_RAW)
std::string headerNameOf(const std::string& var) {
    std::string out;
    bool up = true;
    for (size_t i = 5; i < var.size(); i++) {
        char c = var[i];
        if (c == '_') { out += '-'; up = true; continue; }
        out += up ? c : (char)std::tolower((unsigned char)c);
        up = false;
    }
    return out;
}

std::vector<std::string> exposedEnvNames() {
    std::vector<std::string> out;
    for (char** e = environ; e && *e; e++) {
        std::string kv = *e;
        size_t eq = kv.find('=');
        if (eq == std::string::npos) continue;
        std::string name = kv.substr(0, eq);
        if (exposedEnvName(name)) out.push_back(name);
    }
    for (auto& kv : g_heldHeaders) out.push_back(kv.first);
    std::sort(out.begin(), out.end());
    return out;
}

// the IIS-only names, derived from the CGI environment
const std::vector<std::string>& synthVarNames() {
    static const std::vector<std::string> n = {
        "ALL_HTTP", "ALL_RAW", "APPL_PHYSICAL_PATH", "AUTH_USER", "LOCAL_ADDR",
        "LOGON_USER", "SERVER_PORT_SECURE", "URL"};
    return n;
}

bool serverVariable(const std::string& name, std::string& out) {
    std::string up = name;
    for (auto& ch : up) ch = (char)std::toupper((unsigned char)ch);
    if (up == "ALL_HTTP" || up == "ALL_RAW") {
        out.clear();
        for (auto& n : exposedEnvNames()) {
            if (n.rfind("HTTP_", 0) != 0) continue;
            if (up == "ALL_HTTP") out += n + ":" + envOr(n.c_str()) + "\n";
            else out += headerNameOf(n) + ": " + envOr(n.c_str()) + "\r\n";
        }
        return true;
    }
    if (up == "URL") { out = envOr("SCRIPT_NAME"); return true; }
    if (up == "LOCAL_ADDR") { out = envOr("SERVER_ADDR"); return true; }
    if (up == "AUTH_USER" || up == "LOGON_USER") { out = envOr("REMOTE_USER"); return true; }
    if (up == "SERVER_PORT_SECURE") {
        std::string h = lower(envOr("HTTPS"));
        out = (h == "on" || h == "1") ? "1" : "0";
        return true;
    }
    if (up == "APPL_PHYSICAL_PATH") { out = g_appRoot + "/"; return true; }
    if (!exposedEnvName(up)) return false;
    const char* v = std::getenv(up.c_str());
    if (!v) {
        auto it = g_heldHeaders.find(up);
        if (it == g_heldHeaders.end()) return false;
        out = it->second;
        return true;
    }
    out = v;
    return true;
}

} // namespace

void cgiSanitizeEnv() {
    if (!std::getenv("REQUEST_METHOD")) return; // not CGI: these are the user's own settings
    // Proxy:, Proxy-Auth:, Auth:, Accept:, Referer:, User-Agent: request headers
    // land in variables that fetch(3) and XMLHTTP read as client configuration
    static const char* const vars[] = {"HTTP_PROXY", "HTTP_PROXY_AUTH", "HTTP_AUTH",
                                       "HTTP_ACCEPT", "HTTP_REFERER", "HTTP_USER_AGENT"};
    for (const char* v : vars) {
        if (const char* val = std::getenv(v)) {
            g_heldHeaders[v] = val;
            unsetenv(v);
        }
    }
}

namespace {

// ---------- key/value collection (Request.QueryString, Form, ...) ----------

// Request.Form: parsed on first use (pages that never read the form never
// pay for parsing it), shared by Request.Form and Request("name")
struct LazyFields {
    std::function<std::map<std::string, std::string>()> load;
    std::optional<std::map<std::string, std::string>> data;
    const std::map<std::string, std::string>& get() {
        if (!data) data = load();
        return *data;
    }
};

struct LazyColl : MapObject {
    std::shared_ptr<LazyFields> f;
    explicit LazyColl(std::shared_ptr<LazyFields> fields, const std::string& k) : f(std::move(fields)) {
        kind = k;
        onInvoke = [f = f](MapObject*, std::vector<Variant>& a) -> Variant {
            if (a.empty()) throwErr(450, errDesc(450));
            auto& d = f->get();
            auto it = d.find(lower(toStr(a[0])));
            return Variant::mkStr(it == d.end() ? "" : it->second);
        };
        methods["item"] = onInvoke;
        onEnum = [f = f](MapObject*) -> std::vector<Variant> {
            std::vector<Variant> out;
            for (auto& kv : f->get()) out.push_back(Variant::mkStr(kv.first));
            return out;
        };
    }
    bool hasMember(const std::string& n) const override { return n == "count" || MapObject::hasMember(n); }
    Variant getMember(const std::string& n) override {
        if (n == "count") return Variant::mkLong((long long)f->get().size());
        return MapObject::getMember(n);
    }
};

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
    c->methods["item"] = c->onInvoke; // collection .Item(key), same as the default call
    return c;
}

// ---------- SQLite state store (Session / Application) ----------

std::string g_appNs; // namespace for Application keys (the application root dir)

std::string stateDbPath() {
    const char* env = std::getenv("MScript_ASP_STATE_DB");
    if (env && *env) return env;
    return "/var/db/mscript/asp-state.db";
}

// A private state directory: refuse anything another local user could have
// prepared in advance (a symlink, a directory they own, or one they can
// write to). Our own directory with read-only group/other bits is tightened.
bool privateDir(const std::string& dir) {
    if (mkdir(dir.c_str(), 0700) != 0 && errno != EEXIST) return false;
    struct stat st;
    if (lstat(dir.c_str(), &st) != 0) return false;
    if (!S_ISDIR(st.st_mode) || st.st_uid != geteuid()) return false;
    if (st.st_mode & 022) return false; // was writable by others: may hold planted files
    if ((st.st_mode & 077) && chmod(dir.c_str(), 0700) != 0) return false;
    return true;
}

std::string g_dbPath; // the state db actually opened (the lock file sits next to it)

sqlite3* stateDb() {
    static sqlite3* db = []() -> sqlite3* {
        std::string path = stateDbPath();
        const char* env = std::getenv("MScript_ASP_STATE_DB");
        // the umask only covers creating the state files; restored below so
        // files the script itself creates keep the normal permissions
        // (SQLite gives its -wal/-shm files the db file's own mode)
        mode_t oldMask = umask(0077);
        auto open = [](const std::string& p, int extra) -> sqlite3* {
            sqlite3* d = nullptr;
            if (sqlite3_open_v2(p.c_str(), &d, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | extra,
                                nullptr) != SQLITE_OK) {
                if (d) sqlite3_close(d);
                return nullptr;
            }
            return d;
        };
        sqlite3* d = nullptr;
        if (env && *env) {
            // administrator-chosen location
            std::error_code ec;
            std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
            d = open(path, 0);
        } else if (privateDir("/var/db/mscript")) {
            d = open(path, SQLITE_OPEN_NOFOLLOW);
        }
        if (!d) {
            // per-user private fallback; the name is predictable, so it is only
            // used when privateDir verifies we own it exclusively, and the db
            // file itself must not be a symlink
            std::string dir = "/tmp/mscript-asp-" + std::to_string((long long)geteuid());
            if (privateDir(dir)) {
                path = dir + "/asp-state.db";
                d = open(path, SQLITE_OPEN_NOFOLLOW);
            }
        }
        umask(oldMask);
        if (!d) return nullptr; // fail closed: state calls raise "database unavailable"
        g_dbPath = path;
        chmod(path.c_str(), 0600);
        // concurrent CGI requests share this file: wait for a competing
        // writer instead of failing immediately with SQLITE_BUSY
        sqlite3_busy_timeout(d, 10000);
        sqlite3_exec(d,
            "PRAGMA journal_mode=WAL;"
            "CREATE TABLE IF NOT EXISTS app(k TEXT PRIMARY KEY, v TEXT);"
            "CREATE TABLE IF NOT EXISTS sessions(sid TEXT, k TEXT, v TEXT, PRIMARY KEY(sid, k));"
            "CREATE TABLE IF NOT EXISTS session_meta(sid TEXT PRIMARY KEY, last REAL,"
            "                                        lcid INTEGER, codepage INTEGER,"
            "                                        timeout INTEGER);"
            // abandoned session ids that must never be written again
            "CREATE TABLE IF NOT EXISTS dead(sid TEXT PRIMARY KEY, until REAL);"
            // app-level markers (e.g. the Application_OnStart claim), kept out
            // of the app table so Application.Contents never exposes them
            "CREATE TABLE IF NOT EXISTS meta(k TEXT PRIMARY KEY, v TEXT);",
            nullptr, nullptr, nullptr);
        // older databases predate the lcid/codepage columns
        sqlite3_exec(d, "ALTER TABLE session_meta ADD COLUMN lcid INTEGER;", nullptr, nullptr, nullptr);
        sqlite3_exec(d, "ALTER TABLE session_meta ADD COLUMN codepage INTEGER;", nullptr, nullptr, nullptr);
        sqlite3_exec(d, "ALTER TABLE session_meta ADD COLUMN timeout INTEGER;", nullptr, nullptr, nullptr);
        return d;
    }();
    return db;
}

[[noreturn]] void stateFail(const char* what) {
    throw VBSError(1001, std::string("ASP state: ") + what + " failed");
}

// app rows are namespaced by the ASP application (script name) so independent
// sites sharing one database do not collide
// The namespace prefix "<ns>|" with '%' and '|' percent-escaped in <ns>, so
// the first '|' always ends the namespace: an app directory containing '|'
// can no longer alias (or prefix-match in Count/RemoveAll) another app's
// keys. Ordinary paths are unchanged, so existing Application rows and the
// onstart marker keep their keys.
std::string nsPrefix() {
    std::string out;
    for (char c : g_appNs) {
        if (c == '%') out += "%25";
        else if (c == '|') out += "%7C";
        else out += c;
    }
    return out + "|";
}
std::string appKey(const std::string& k) { return nsPrefix() + k; }

// session rows are keyed by application too: a session id issued by one app
// is unknown to every other app sharing the state database
std::string sessKey(const std::string& sid) { return nsPrefix() + sid; }
void bindSid(sqlite3_stmt* st, int idx, const std::string& sid) {
    std::string k = sessKey(sid);
    sqlite3_bind_text(st, idx, k.data(), (int)k.size(), SQLITE_TRANSIENT);
}

// per-application session cookie name (like IIS's ASPSESSIONIDxxxxxxxx), so
// apps on one host never overwrite each other's session cookie
std::string sessionCookieName() {
    uint32_t h = 2166136261u; // FNV-1a of the app namespace
    for (unsigned char c : g_appNs) { h ^= c; h *= 16777619u; }
    char buf[16];
    std::snprintf(buf, sizeof buf, "%08x", h);
    return std::string("MScriptSessionID_") + buf;
}

// an abandoned session id is dead: reads see nothing and writes are dropped,
// so an in-flight request holding the old id cannot recreate its state
bool sessionIsDead(const std::string& sid) {
    sqlite3* db = stateDb();
    if (!db) return false;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT 1 FROM dead WHERE sid=?1", -1, &st, nullptr) != SQLITE_OK)
        return false;
    bindSid(st, 1, sid);
    bool dead = sqlite3_step(st) == SQLITE_ROW;
    sqlite3_finalize(st);
    return dead;
}

void sessionMarkDead(const std::string& sid) {
    sqlite3* db = stateDb();
    if (!db) stateFail("database unavailable");
    sqlite3_exec(db, "DELETE FROM dead WHERE until < strftime('%s','now');", nullptr, nullptr, nullptr);
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db, "INSERT OR REPLACE INTO dead(sid, until) VALUES(?1, ?2)", -1, &st, nullptr) != SQLITE_OK)
        stateFail("prepare");
    bindSid(st, 1, sid);
    sqlite3_bind_double(st, 2, (double)std::time(nullptr) + 86400.0);
    if (sqlite3_step(st) != SQLITE_DONE) stateFail("mark dead");
    sqlite3_finalize(st);
}

std::string stateGet(bool isSession, const std::string& sid, const std::string& k) {
    if (isSession && sessionIsDead(sid)) return "";
    sqlite3* db = stateDb();
    if (!db) return "";
    sqlite3_stmt* st = nullptr;
    const char* sql = isSession
        ? "SELECT v FROM sessions WHERE sid=?1 AND k=?2"
        : "SELECT v FROM app WHERE k=?1";
    if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) != SQLITE_OK) return "";
    if (isSession) {
        bindSid(st, 1, sid);
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
    if (isSession && sessionIsDead(sid)) return; // abandoned id stays dead
    sqlite3* db = stateDb();
    if (!db) stateFail("database unavailable");
    sqlite3_stmt* st = nullptr;
    const char* sql = isSession
        ? "INSERT OR REPLACE INTO sessions(sid, k, v) VALUES(?1, ?2, ?3)"
        : "INSERT OR REPLACE INTO app(k, v) VALUES(?1, ?2)";
    if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) != SQLITE_OK) stateFail("prepare");
    if (isSession) {
        bindSid(st, 1, sid);
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
        bindSid(st, 1, sid);
        sqlite3_step(st);
        sqlite3_finalize(st);
    }
    if (sqlite3_prepare_v2(db, "DELETE FROM session_meta WHERE sid=?1", -1, &st, nullptr) == SQLITE_OK) {
        bindSid(st, 1, sid);
        sqlite3_step(st);
        sqlite3_finalize(st);
    }
}

void stateRemove(bool isSession, const std::string& sid, const std::string& k) {
    sqlite3* db = stateDb();
    if (!db) stateFail("database unavailable");
    sqlite3_stmt* st = nullptr;
    const char* sql = isSession
        ? "DELETE FROM sessions WHERE sid=?1 AND k=?2"
        : "DELETE FROM app WHERE k=?1";
    if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) != SQLITE_OK) stateFail("prepare");
    if (isSession) {
        bindSid(st, 1, sid);
        sqlite3_bind_text(st, 2, k.data(), (int)k.size(), SQLITE_TRANSIENT);
    } else {
        std::string ak = appKey(k);
        sqlite3_bind_text(st, 1, ak.data(), (int)ak.size(), SQLITE_TRANSIENT);
    }
    if (sqlite3_step(st) != SQLITE_DONE) stateFail("remove");
    sqlite3_finalize(st);
}

long long stateCount(bool isSession, const std::string& sid) {
    sqlite3* db = stateDb();
    if (!db) stateFail("database unavailable");
    sqlite3_stmt* st = nullptr;
    // instr() prefix match avoids LIKE wildcard issues in the app namespace
    const char* sql = isSession
        ? "SELECT COUNT(*) FROM sessions WHERE sid=?1"
        : "SELECT COUNT(*) FROM app WHERE instr(k, ?1)=1";
    if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) != SQLITE_OK) stateFail("prepare");
    if (isSession) {
        bindSid(st, 1, sid);
    } else {
        std::string ns = nsPrefix();
        sqlite3_bind_text(st, 1, ns.data(), (int)ns.size(), SQLITE_TRANSIENT);
    }
    long long n = 0;
    if (sqlite3_step(st) != SQLITE_ROW) stateFail("count");
    n = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    return n;
}

// Contents.RemoveAll: clears keys only — for a session the metadata row must
// survive, otherwise the live session would look expired on the next access
void stateRemoveAllKeys(bool isSession, const std::string& sid) {
    sqlite3* db = stateDb();
    if (!db) stateFail("database unavailable");
    sqlite3_stmt* st = nullptr;
    const char* sql = isSession
        ? "DELETE FROM sessions WHERE sid=?1"
        : "DELETE FROM app WHERE instr(k, ?1)=1";
    if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) != SQLITE_OK) stateFail("prepare");
    if (isSession) {
        bindSid(st, 1, sid);
    } else {
        std::string ns = nsPrefix();
        sqlite3_bind_text(st, 1, ns.data(), (int)ns.size(), SQLITE_TRANSIENT);
    }
    if (sqlite3_step(st) != SQLITE_DONE) stateFail("removeall");
    sqlite3_finalize(st);
}

void sessionTouch(const std::string& sid) {
    if (sessionIsDead(sid)) return;
    sqlite3* db = stateDb();
    if (!db) stateFail("database unavailable");
    sqlite3_stmt* st = nullptr;
    // upsert: keep any lcid/codepage columns already on the row
    if (sqlite3_prepare_v2(db,
            "INSERT INTO session_meta(sid, last) VALUES(?1, ?2) "
            "ON CONFLICT(sid) DO UPDATE SET last=?2", -1, &st, nullptr) != SQLITE_OK)
        stateFail("prepare");
    bindSid(st, 1, sid);
    sqlite3_bind_double(st, 2, (double)std::time(nullptr));
    if (sqlite3_step(st) != SQLITE_DONE) stateFail("touch");
    sqlite3_finalize(st);
}

constexpr long long DEFAULT_SESSION_TIMEOUT = 20; // minutes, as IIS

enum class SessState {
    Live,    // exists, not expired: touched
    Expired, // existed and timed out: claimed by this process (meta row
             // deleted, contents kept for Session_OnEnd; sessionFinishEnd
             // removes them)
    Invalid, // unknown, abandoned, or already claimed by another request
};

// One write transaction: the expiry check uses the session's own persisted
// timeout, and the claim (deleting the meta row) means at most one process
// ever fires Session_OnEnd for a session; a concurrent touch can neither
// resurrect it nor race the claim. *timeoutOut receives the session timeout.
SessState sessionCheck(const std::string& sid, long long* timeoutOut) {
    sqlite3* db = stateDb();
    if (!db) stateFail("database unavailable");
    if (timeoutOut) *timeoutOut = DEFAULT_SESSION_TIMEOUT;
    if (sessionIsDead(sid)) return SessState::Invalid;
    if (sqlite3_exec(db, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) != SQLITE_OK)
        stateFail("begin");
    auto rollbackFail = [db](const char* what) {
        sqlite3_exec(db, "ROLLBACK;", nullptr, nullptr, nullptr);
        stateFail(what);
    };
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT last, timeout FROM session_meta WHERE sid=?1", -1, &st, nullptr) != SQLITE_OK)
        rollbackFail("prepare");
    bindSid(st, 1, sid);
    int rc = sqlite3_step(st);
    SessState state = SessState::Invalid; // no metadata row: never issued (or already ended)
    if (rc == SQLITE_ROW) {
        double last = sqlite3_column_double(st, 0);
        long long tmo = sqlite3_column_type(st, 1) == SQLITE_NULL ? DEFAULT_SESSION_TIMEOUT
                                                                   : sqlite3_column_int64(st, 1);
        if (tmo < 1) tmo = DEFAULT_SESSION_TIMEOUT;
        if (timeoutOut) *timeoutOut = tmo;
        state = ((double)std::time(nullptr) - last) > (double)tmo * 60.0 ? SessState::Expired
                                                                         : SessState::Live;
    }
    sqlite3_finalize(st);
    if (rc != SQLITE_ROW && rc != SQLITE_DONE) rollbackFail("expiry check");
    const char* sql = state == SessState::Expired ? "DELETE FROM session_meta WHERE sid=?1"
                    : state == SessState::Live    ? "UPDATE session_meta SET last=?2 WHERE sid=?1"
                                                  : nullptr;
    if (sql) {
        if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) != SQLITE_OK) rollbackFail("prepare");
        bindSid(st, 1, sid);
        if (state == SessState::Live) sqlite3_bind_double(st, 2, (double)std::time(nullptr));
        rc = sqlite3_step(st);
        sqlite3_finalize(st);
        if (rc != SQLITE_DONE) rollbackFail("expire");
    }
    if (sqlite3_exec(db, "COMMIT;", nullptr, nullptr, nullptr) != SQLITE_OK) rollbackFail("commit");
    return state;
}

// Opportunistic cleanup (about 1 request in 64): sessions past their own
// timeout, contents left without a meta row (a crash between claim and
// cleanup), and expired abandoned-id markers. Each CGI request is its own
// process, so there is no timer: Session_OnEnd fires only when an expired
// session's cookie comes back. Never fails the request.
void sessionGc() {
    unsigned char r = 0;
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return;
    ssize_t got = read(fd, &r, 1);
    close(fd);
    if (got != 1 || (r & 63) != 0) return;
    sqlite3* db = stateDb();
    if (!db) return;
    if (sqlite3_exec(db, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) != SQLITE_OK) return;
    const char* sql =
        "DELETE FROM session_meta WHERE last < CAST(strftime('%s','now') AS REAL)"
        "  - 60.0 * COALESCE(NULLIF(timeout, 0), 20);"
        "DELETE FROM sessions WHERE sid NOT IN (SELECT sid FROM session_meta);"
        "DELETE FROM dead WHERE until < CAST(strftime('%s','now') AS REAL);";
    if (sqlite3_exec(db, sql, nullptr, nullptr, nullptr) == SQLITE_OK)
        sqlite3_exec(db, "COMMIT;", nullptr, nullptr, nullptr);
    else
        sqlite3_exec(db, "ROLLBACK;", nullptr, nullptr, nullptr);
}

// persisted per-session settings (lcid/codepage); col is an internal constant
void sessionSetProp(const std::string& sid, const char* col, long long v) {
    sqlite3* db = stateDb();
    if (!db) stateFail("database unavailable");
    std::string sql = std::string("INSERT INTO session_meta(sid, last, ") + col + ") VALUES(?1, ?2, ?3) "
                      "ON CONFLICT(sid) DO UPDATE SET " + col + "=?3";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) stateFail("prepare");
    bindSid(st, 1, sid);
    sqlite3_bind_double(st, 2, (double)std::time(nullptr));
    sqlite3_bind_int64(st, 3, v);
    if (sqlite3_step(st) != SQLITE_DONE) stateFail("set prop");
    sqlite3_finalize(st);
}

long long sessionGetProp(const std::string& sid, const char* col, long long dflt) {
    sqlite3* db = stateDb();
    if (!db) return dflt;
    std::string sql = std::string("SELECT ") + col + " FROM session_meta WHERE sid=?1";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) return dflt;
    bindSid(st, 1, sid);
    long long out = dflt;
    if (sqlite3_step(st) == SQLITE_ROW && sqlite3_column_type(st, 0) != SQLITE_NULL)
        out = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    return out;
}

// atomically claim an app-level marker in the separate meta table (never
// visible through Application.Contents); true if this caller claimed it
bool metaClaim(const std::string& k) {
    sqlite3* db = stateDb();
    if (!db) stateFail("database unavailable");
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db, "INSERT OR IGNORE INTO meta(k, v) VALUES(?1, '1')", -1, &st, nullptr) != SQLITE_OK)
        stateFail("prepare");
    sqlite3_bind_text(st, 1, k.data(), (int)k.size(), SQLITE_TRANSIENT);
    if (sqlite3_step(st) != SQLITE_DONE) stateFail("claim");
    int changed = sqlite3_changes(db);
    sqlite3_finalize(st);
    return changed == 1;
}

// cross-process lock for Application.Lock/Unlock (flock on a file next to the db)
int appLockFd() {
    static int fd = []() -> int {
        if (!stateDb() || g_dbPath.empty()) return -1;
        // next to the db actually in use; never follow a planted symlink
        std::string path = g_dbPath + ".lock";
        return open(path.c_str(), O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
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

// the session cookie: HttpOnly (no script access), SameSite=Lax, and Secure
// whenever the request arrived over TLS
void setSessionCookie(const std::string& sid) {
    if (!g_resp) return;
    const std::string name = sessionCookieName();
    auto ck = std::make_shared<CookieObj>(lower(name));
    ck->value = sid;
    ck->props["path"] = Variant::mkStr("/");
    ck->props["httponly"] = Variant::mkBool(true);
    ck->props["samesite"] = Variant::mkStr("Lax");
    const char* https = std::getenv("HTTPS");
    if (https && (lower(https) == "on" || std::string(https) == "1"))
        ck->props["secure"] = Variant::mkBool(true);
    // replace any same-named (case-insensitive) entry
    for (auto it = g_resp->outCookies.begin(); it != g_resp->outCookies.end();) {
        if (lower(it->first) == lower(name)) it = g_resp->outCookies.erase(it);
        else ++it;
    }
    g_resp->outCookies[name] = ck;
    g_resp->dirtyCookies.insert(lower(name));
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

// Session.Contents / Application.Contents: the documented collection view
// (Count, Item, Remove, RemoveAll) over the same sqlite-backed store
std::shared_ptr<MapObject> makeContentsColl(bool isSession, std::shared_ptr<std::string> sid) {
    auto c = std::make_shared<MapObject>();
    c->kind = "Contents";
    c->onInvoke = [isSession, sid](MapObject*, std::vector<Variant>& a) -> Variant {
        return fromJsonText(stateGet(isSession, isSession ? *sid : "", stateKeyOf(a)));
    };
    c->onAssignInvoke = [isSession, sid](MapObject*, std::vector<Variant>& a, const Variant& v) {
        stateSet(isSession, isSession ? *sid : "", stateKeyOf(a), toJsonText(v));
    };
    c->methods["item"] = c->onInvoke;
    c->methods["count"] = [isSession, sid](MapObject*, std::vector<Variant>&) -> Variant {
        return Variant::mkLong(stateCount(isSession, isSession ? *sid : ""));
    };
    c->methods["remove"] = [isSession, sid](MapObject*, std::vector<Variant>& a) -> Variant {
        stateRemove(isSession, isSession ? *sid : "", stateKeyOf(a));
        return Variant();
    };
    c->methods["removeall"] = [isSession, sid](MapObject*, std::vector<Variant>&) -> Variant {
        stateRemoveAllKeys(isSession, isSession ? *sid : "");
        return Variant();
    };
    return c;
}

// Session object: dict-style contents plus the LCID/CodePage properties.
// LCID drives the same locale state as SetLocale/GetLocale; both persist in
// session_meta so the next request with the same cookie restores them.
struct SessionObj : MapObject {
    Interp* ip;
    std::shared_ptr<std::string> sid;
    long long codePage = 1252; // Windows-1252 default
    SessionObj(Interp& i, std::shared_ptr<std::string> s) : ip(&i), sid(std::move(s)) {}
    Variant getMember(const std::string& n) override {
        if (n == "lcid") return ip->localeId;
        if (n == "codepage") return Variant::mkLong(codePage);
        return MapObject::getMember(n);
    }
    void setMember(const std::string& n, const Variant& v) override {
        if (n == "lcid") {
            long long l = toInt64(v);
            if (l < 0 || l > 0x7FFFFFFFLL) throwErr(5, errDesc(5));
            ip->localeId = Variant::mkLong(l);
            sessionSetProp(*sid, "lcid", l);
            return;
        }
        if (n == "codepage") {
            codePage = toInt64(v);
            g_sessionCodePage = codePage; // affects output charset/encoding
            sessionSetProp(*sid, "codepage", codePage);
            return;
        }
        if (n == "timeout") {
            // minutes, 1..1440 as in IIS; persisted so later requests expire
            // the session on its own timeout (0 used to mean "never expires")
            long long t = toInt64(v);
            if (t < 1 || t > 1440) throwErr(5, errDesc(5));
            sessionSetProp(*sid, "timeout", t);
            MapObject::setMember(n, Variant::mkLong(t));
            return;
        }
        if (n == "sessionid") throwErr(438, errDesc(438)); // read-only
        MapObject::setMember(n, v);
    }
};

// Response object: IsClientConnected is read-only (CGI cannot detect
// disconnects, so it is always True and assignment raises 438)
struct RespObj : MapObject {
    void setMember(const std::string& n, const Variant& v) override {
        if (n == "isclientconnected") throwErr(438, errDesc(438));
        // properties that only reach the client as headers
        static const std::set<std::string> headerProps = {
            "status", "contenttype", "charset", "cachecontrol", "expires", "expiresabsolute", "pics"};
        if (headerProps.count(n)) {
            requireHeadersUnsent();
            // validate at the assignment, not at header time
            if (n == "expires") (void)toInt64(v);
            else if (!(n == "expiresabsolute" && v.t == VT::Date)) cleanHeaderText(toStr(v), n.c_str());
        }
        MapObject::setMember(n, v);
    }
};

// per-request session state shared by the Session object, Abandon, the
// request-start expiry check and the global.asa hooks
struct SessCtx {
    Interp* ip = nullptr;
    MapObject* sess = nullptr;
    std::shared_ptr<std::string> sid;
    std::string pendingEnd; // expired session claimed at request start
    bool inOnEnd = false;   // Session_OnEnd running: no expiry checks/touches
    bool ended = false;     // Session_OnEnd already fired this request
};
SessCtx g_sess;

// Fire Session_OnEnd for a session this process claimed (or is abandoning),
// with that session's contents still readable through Session, then destroy
// it. Request/Response are hidden during the hook, as in IIS, so it can
// neither write into nor redirect/end the current visitor's response; its
// errors (and End) are swallowed so they cannot break the visitor's request.
void runSessionOnEnd(const std::string& oldSid) {
    SessCtx& c = g_sess;
    if (c.ip && c.sid && !c.ended) {
        c.ended = true;
        std::string cur = *c.sid;
        *c.sid = oldSid;
        if (c.sess) c.sess->props["sessionid"] = Variant::mkStr(oldSid);
        auto& gv = c.ip->global.vars;
        std::vector<std::pair<std::string, Variant>> hidden;
        for (const char* k : {"request", "response"}) {
            auto it = gv.find(k);
            if (it != gv.end()) { hidden.emplace_back(k, it->second); gv.erase(it); }
        }
        c.inOnEnd = true;
        g_suppressOutput = true;
        try {
            c.ip->callGlobalProc("session_onend");
        } catch (VBSError&) {
        } catch (EndSig&) {
        }
        g_suppressOutput = false;
        c.inOnEnd = false;
        for (auto& [k, v] : hidden) gv[k] = v;
        *c.sid = cur;
        if (c.sess) c.sess->props["sessionid"] = Variant::mkStr(cur);
    }
    stateRemoveAll(oldSid);
    sessionMarkDead(oldSid); // in-flight requests with the old id cannot write it back
}

// replace the current session with a brand-new id (expired or abandoned)
void rotateSession() {
    SessCtx& c = g_sess;
    *c.sid = newSessionId();
    if (c.sess) c.sess->props["sessionid"] = Variant::mkStr(*c.sid);
    if (c.sess) c.sess->props["timeout"] = Variant::mkLong(DEFAULT_SESSION_TIMEOUT);
    setSessionCookie(*c.sid);
}

// stateful collection object backed by the sqlite store
void fillStateColl(Interp& ip, MapObject* c, bool isSession, std::shared_ptr<std::string> sid) {
    c->kind = isSession ? "Session" : "Application";
    if (isSession) c->props["sessionid"] = Variant::mkStr(*sid);
    if (isSession) c->props["timeout"] = Variant::mkLong(DEFAULT_SESSION_TIMEOUT);
    c->props["contents"] = Variant::mkObj(makeContentsColl(isSession, sid));
    c->onInvoke = [isSession, sid](MapObject*, std::vector<Variant>& a) -> Variant {
        std::string k = stateKeyOf(a);
        if (isSession) {
            // inside Session_OnEnd the (claimed) old session is read as-is:
            // checking or touching it would resurrect it
            if (g_sess.inOnEnd) return fromJsonText(stateGet(true, *sid, k));
            SessState st = sessionCheck(*sid, nullptr);
            if (st != SessState::Live) {
                // expired (or ended elsewhere) while this request ran: the
                // replacement session starts fresh under a NEW id
                std::string old = *sid;
                if (st == SessState::Expired) runSessionOnEnd(old);
                rotateSession();
                sessionTouch(*sid);
                return Variant();
            }
            return fromJsonText(stateGet(true, *sid, k));
        }
        return fromJsonText(stateGet(false, "", k));
    };
    c->onAssignInvoke = [isSession, sid](MapObject*, std::vector<Variant>& a, const Variant& v) {
        std::string k = stateKeyOf(a);
        if (isSession && !g_sess.inOnEnd) sessionTouch(*sid);
        stateSet(isSession, isSession ? *sid : "", k, toJsonText(v));
    };
    if (isSession) {
        c->methods["abandon"] = [&ip](MapObject*, std::vector<Variant>&) -> Variant {
            if (g_sess.inOnEnd) return Variant(); // the session is already ending
            std::string old = *g_sess.sid;
            runSessionOnEnd(old);
            // rotate: the abandoned id must not be reused; the replacement is
            // a new session and gets Session_OnStart
            rotateSession();
            ip.aspNewSession = true;
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
}

} // namespace

// The ASP application root is the nearest ancestor directory of the script
// (including its own directory) that contains a global.asa; if none does,
// the script's own directory. Pages in subdirectories share their app root's
// global.asa and Application namespace.
std::string aspAppRootOf(const std::string& scriptFullName) {
    std::error_code ec;
    std::filesystem::path dir = std::filesystem::absolute(scriptFullName, ec).parent_path();
    std::filesystem::path p = dir;
    for (;;) {
        if (std::filesystem::exists(p / "global.asa", ec)) return p.lexically_normal().string();
        if (!p.has_parent_path() || p == p.parent_path()) break;
        p = p.parent_path();
    }
    return dir.lexically_normal().string();
}

void registerAspObjects(Interp& ip, bool cgi) {
    (void)cgi;
    // Application values are namespaced by the application root (the nearest
    // ancestor with a global.asa), so all pages of one app share state and
    // distinct apps never collide — even with same-named pages
    g_appRoot = aspAppRootOf(ip.scriptFullName);
    g_appNs = lower(g_appRoot);

    // ---- Response ----
    g_resp = std::make_shared<AspResponse>();
    auto resp = std::make_shared<RespObj>();
    resp->kind = "Response";
    ip.responseObj = resp;
    g_respObj = resp.get();
    resp->props["buffer"] = Variant::mkBool(true);
    resp->props["isclientconnected"] = Variant::mkBool(true); // CGI cannot detect disconnects
    resp->props["contenttype"] = Variant::mkStr("text/html");
    resp->props["status"] = Variant::mkStr("200 OK");
    resp->methods["write"] = [](MapObject*, std::vector<Variant>& a) -> Variant {
        for (auto& v : a) responseBody(toStr(v));
        return Variant();
    };
    resp->methods["binarywrite"] = [](MapObject*, std::vector<Variant>& a) -> Variant {
        for (auto& v : a) {
            if (v.t == VT::Array && v.arr) {
                // each element is a raw byte, not decimal text; binary output
                // bypasses the code-page transcoding
                std::string bytes;
                bytes.reserve(v.arr->data.size());
                for (auto& e : v.arr->data) bytes += (char)(unsigned char)toInt64(e);
                responseBody(bytes, true);
            } else {
                responseBody(toStr(v), true);
            }
        }
        return Variant();
    };
    resp->methods["flush"] = [](MapObject*, std::vector<Variant>&) -> Variant {
        if (g_resp) {
            syncRespProps();
            flushBody(); // same transcoding as the final flush
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
        requireHeadersUnsent();
        if (g_resp) {
            std::string url = cleanHeaderText(toStr(a[0]), "redirect URL");
            // the status must also go into the property: syncRespProps
            // re-reads Response.Status at header time
            g_resp->status = "302 Object moved";
            if (g_respObj) g_respObj->props["status"] = Variant::mkStr(g_resp->status);
            g_resp->headers.push_back({"Location", url});
            // like IIS, discard the buffered page: content written before a
            // redirect (often what an unauthenticated user must not see)
            // is never sent
            g_resp->body.clear();
            g_resp->body.push_back({"<head><title>Object moved</title></head>\r\n<body><h1>Object Moved</h1>"
                                    "This object may be found <a HREF=\"" + htmlEncode(url) +
                                    "\">here</a>.</body>\r\n", false});
            g_resp->ended = true;
        }
        throw EndSig{};
    };
    resp->methods["addheader"] = [](MapObject*, std::vector<Variant>& a) -> Variant {
        if (a.size() < 2) throwErr(450, errDesc(450));
        requireHeadersUnsent();
        if (g_resp)
            g_resp->headers.push_back({cleanHeaderText(toStr(a[0]), "header name"),
                                       cleanHeaderText(toStr(a[1]), "header value")});
        return Variant();
    };

    // Response.Cookies("name") returns a per-name cookie object whose props
    // carry value/expires/path/domain/secure/httponly; Set-Cookie is built at
    // header time. Response.Cookies("name") = value sets the value.
    auto cookies = std::make_shared<MapObject>();
    cookies->kind = "Cookies";
    auto reqCookies = parseCookies(std::getenv("HTTP_COOKIE") ? std::getenv("HTTP_COOKIE") : "");
    // keys keep the original casing; lookup is case-insensitive
    auto cookieFor = [](const std::string& name) -> std::shared_ptr<CookieObj> {
        if (!g_resp) return nullptr;
        std::string disp = cleanCookieName(name);
        std::string k = lower(disp);
        for (auto& [existing, ck] : g_resp->outCookies)
            if (lower(existing) == k) return ck;
        auto ck = std::make_shared<CookieObj>(k);
        g_resp->outCookies[disp] = ck;
        return ck;
    };
    cookies->onInvoke = [cookieFor](MapObject*, std::vector<Variant>& a) -> Variant {
        if (a.empty()) throwErr(450, errDesc(450));
        auto ck = cookieFor(toStr(a[0]));
        if (!ck) throwErr(5, errDesc(5));
        return Variant::mkObj(ck);
    };
    // Response.Cookies("n") = v sets the scalar value (dropping any sub-keys)
    cookies->onAssignInvoke = [cookieFor](MapObject*, std::vector<Variant>& a, const Variant& v) {
        if (a.empty()) throwErr(450, errDesc(450));
        auto ck = cookieFor(toStr(a[0]));
        if (ck) {
            ck->value = toStr(v);
            ck->subkeys.clear();
            ck->markDirty();
        }
    };
    resp->props["cookies"] = Variant::mkObj(cookies);

    // ---- Request ----
    auto req = std::make_shared<MapObject>();
    req->kind = "Request";
    auto qs = parsePairs(std::getenv("QUERY_STRING") ? std::getenv("QUERY_STRING") : "");
    req->props["querystring"] = Variant::mkObj(makeColl(qs, "QueryString"));
    auto form = std::make_shared<LazyFields>();
    form->load = [] {
        std::map<std::string, std::string> m;
        const char* rm = std::getenv("REQUEST_METHOD");
        const char* ct = std::getenv("CONTENT_TYPE");
        if (rm && !strcmp(rm, "POST") && ct) {
            std::string lct = lower(ct); // media types are case-insensitive
            const std::string& body = requestBody(); // read once, capped
            if (lct.rfind("application/x-www-form-urlencoded", 0) == 0)
                m = parsePairs(body);
            else if (lct.rfind("multipart/form-data", 0) == 0)
                m = parseMultipart(body, ct);
        }
        return m;
    };
    req->props["form"] = Variant::mkObj(std::make_shared<LazyColl>(form, "Form"));
    req->props["cookies"] = Variant::mkObj(makeReqCookies(reqCookies));
    // Request("name"): combined lookup in the documented order —
    // QueryString, Form, Cookies, ServerVariables
    req->onInvoke = [qs, form, reqCookies](MapObject*, std::vector<Variant>& a) -> Variant {
        if (a.empty()) throwErr(450, errDesc(450));
        std::string k = lower(toStr(a[0]));
        auto it = qs.find(k);
        if (it != qs.end()) return Variant::mkStr(it->second);
        auto& fd = form->get();
        it = fd.find(k);
        if (it != fd.end()) return Variant::mkStr(it->second);
        it = reqCookies.find(k);
        if (it != reqCookies.end()) return Variant::mkStr(urlDecode(it->second));
        std::string v;
        return Variant::mkStr(serverVariable(toStr(a[0]), v) ? v : "");
    };
    auto sv = std::make_shared<MapObject>();
    sv->kind = "ServerVariables";
    sv->onInvoke = [](MapObject*, std::vector<Variant>& a) -> Variant {
        if (a.empty()) throwErr(450, errDesc(450));
        std::string v;
        return Variant::mkStr(serverVariable(toStr(a[0]), v) ? v : "");
    };
    sv->methods["item"] = sv->onInvoke;
    // For Each name In Request.ServerVariables: the exposed variables present
    // plus the synthesized IIS names
    auto svNames = [] {
        std::vector<std::string> n = exposedEnvNames();
        for (auto& x : synthVarNames()) n.push_back(x);
        std::sort(n.begin(), n.end());
        n.erase(std::unique(n.begin(), n.end()), n.end());
        return n;
    };
    sv->onEnum = [svNames](MapObject*) -> std::vector<Variant> {
        std::vector<Variant> out;
        for (auto& n : svNames()) out.push_back(Variant::mkStr(n));
        return out;
    };
    sv->props["count"] = Variant::mkLong((long long)svNames().size());
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
        if (len <= 0) return Variant::mkStr("");
        // a byte string, as ASP pages consume it (LenB/MidB/InStrB/AscB):
        // one byte per byte, where a Variant array took ~90 bytes per byte
        // (1.5 GB for a 16 MiB upload)
        std::string out = body.substr(g_reqBodyPos, (size_t)len);
        g_reqBodyPos += (size_t)len;
        return Variant::mkStr(std::move(out));
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
        return Variant::mkStr(htmlEncode(toStr(a[0])));
    };
    srv->methods["urlencode"] = [](MapObject*, std::vector<Variant>& a) -> Variant {
        if (a.empty()) return Variant::mkStr("");
        return Variant::mkStr(urlEncode(toStr(a[0])));
    };
    // Virtual-path resolution shared by MapPath and Execute/Transfer:
    //   "/x"  -> DOCUMENT_ROOT/x (the app root when DOCUMENT_ROOT is unset)
    //   "x"   -> relative to the page's own directory
    // The result must stay inside DOCUMENT_ROOT (absolute virtual paths) or
    // the application root (relative paths); ".." escaping it is refused like
    // IIS's default "parent paths disabled" (ASP 0131).
    struct VPath { std::filesystem::path full, root; };
    auto resolveVirtual = [&ip](const std::string& p) -> VPath {
        std::error_code ec;
        if (p.find('\0') != std::string::npos) throwErr(5, errDesc(5));
        std::filesystem::path scriptDir = std::filesystem::absolute(ip.scriptFullName, ec).parent_path();
        std::filesystem::path appRoot = std::filesystem::path(aspAppRootOf(ip.scriptFullName));
        VPath r;
        if (!p.empty() && (p[0] == '/' || p[0] == '\\')) {
            const char* dr = std::getenv("DOCUMENT_ROOT");
            r.root = (dr && *dr) ? std::filesystem::absolute(dr, ec) : appRoot;
            r.full = r.root / p.substr(1);
        } else {
            r.root = appRoot;
            r.full = scriptDir / p;
        }
        r.root = r.root.lexically_normal();
        r.full = r.full.lexically_normal();
        std::string rs = r.root.string(), fs = r.full.string();
        while (rs.size() > 1 && rs.back() == '/') rs.pop_back();
        while (fs.size() > 1 && fs.back() == '/') fs.pop_back();
        if (fs != rs && fs.rfind(rs == "/" ? rs : rs + "/", 0) != 0)
            throw VBSError((int)0x80004005,
                           "Server object error 'ASP 0131 : 80004005': Disallowed Parent Path. "
                           "The path '" + p + "' is outside the application.");
        r.full = fs;
        r.root = rs;
        return r;
    };
    srv->methods["mappath"] = [resolveVirtual](MapObject*, std::vector<Variant>& a) -> Variant {
        if (a.empty()) throwErr(450, errDesc(450));
        return Variant::mkStr(resolveVirtual(toStr(a[0])).full.string());
    };
    // Execute/Transfer: only .asp pages, and the confinement is re-checked on
    // the canonical (symlink-resolved) paths of both sides, so a symlink in
    // the app cannot point outside it and a symlinked app root still works
    auto doExecute = [&ip, resolveVirtual](const std::string& rel) {
        VPath v = resolveVirtual(rel);
        std::string ext = v.full.extension().string();
        if (lower(ext) != ".asp")
            throwErr(5, "ASP Server.Execute/Transfer: only .asp pages can be executed");
        std::error_code ec;
        std::filesystem::path real = std::filesystem::canonical(v.full, ec);
        if (ec) throwErr(53, "File not found: " + v.full.string());
        std::filesystem::path realRoot = std::filesystem::canonical(v.root, ec);
        if (ec) throwErr(53, "File not found: " + v.root.string());
        std::string rs = realRoot.string(), fs = real.string();
        if (fs.rfind(rs == "/" ? rs : rs + "/", 0) != 0)
            throwErr(5, "ASP Server.Execute/Transfer: path outside the application");
        // nesting limit: a page that executes itself (e.g. via
        // Server.Execute Request("page")) used to recurse until the stack
        // overflowed and the process died with no response
        static int depth = 0;
        constexpr int MAX_EXECUTE_DEPTH = 32;
        if (depth >= MAX_EXECUTE_DEPTH)
            throwErr(28, "Out of stack space: Server.Execute/Transfer nested too deeply");
        std::string code;
        {
            // read and close before running: an open stream per nesting level
            // used to exhaust file descriptors
            std::ifstream in(real, std::ios::binary);
            if (!in) throwErr(53, "File not found: " + v.full.string());
            std::ostringstream ss;
            ss << in.rdbuf();
            code = ss.str();
        }
        struct DepthGuard { DepthGuard() { depth++; } ~DepthGuard() { depth--; } } guard;
        ip.executeString(aspToVbs(code), true);
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
    long long timeout = DEFAULT_SESSION_TIMEOUT;
    g_sess = SessCtx{};
    g_sess.ip = &ip;
    g_sess.sid = sid;
    {
        auto it = reqCookies.find(lower(sessionCookieName()));
        // only reuse an id this server issued that is still live: a
        // malformed, unknown (client-chosen), expired or abandoned id is
        // replaced with a fresh one, so an attacker cannot fix a victim's
        // session id in advance
        std::string presented = it == reqCookies.end() ? "" : urlDecode(it->second);
        if (validSessionId(presented)) {
            switch (sessionCheck(presented, &timeout)) {
                case SessState::Live: *sid = presented; break;
                // Session_OnEnd runs from aspGlobalAsaHooks, once global.asa
                // is loaded (it is not yet, here)
                case SessState::Expired: g_sess.pendingEnd = presented; break;
                case SessState::Invalid: break;
            }
        }
    }
    if (sid->empty()) {
        *sid = newSessionId();
        timeout = DEFAULT_SESSION_TIMEOUT;
        ip.aspNewSession = true;
        setSessionCookie(*sid);
    }
    sessionTouch(*sid);
    sessionGc();
    auto session = std::make_shared<SessionObj>(ip, sid);
    g_sess.sess = session.get();
    fillStateColl(ip, session.get(), true, sid);
    session->props["timeout"] = Variant::mkLong(timeout);
    // restore persisted session settings
    {
        long long lcid = sessionGetProp(*sid, "lcid", 0);
        if (lcid > 0) ip.localeId = Variant::mkLong(lcid);
        session->codePage = sessionGetProp(*sid, "codepage", 1252);
        g_sessionCodePage = session->codePage;
    }
    auto app = std::make_shared<MapObject>();
    fillStateColl(ip, app.get(), false, sid);

    ip.global.vars["request"] = Variant::mkObj(req);
    ip.global.vars["response"] = Variant::mkObj(resp);
    ip.global.vars["server"] = Variant::mkObj(srv);
    ip.global.vars["session"] = Variant::mkObj(session);
    ip.global.vars["application"] = Variant::mkObj(app);
}

// global.asa hooks, called once per request after the program is loaded:
// Application_OnStart is guarded in the state db so it runs once per
// application even though each CGI request is a fresh process
void aspGlobalAsaHooks(Interp& ip) {
    // claim the marker atomically BEFORE running the hook: overlapping first
    // requests must not both run Application_OnStart, and a hook error must
    // not leave the marker unset (the claim lives in the separate meta table,
    // never visible through Application.Contents)
    if (metaClaim(nsPrefix() + "onstart")) ip.callGlobalProc("application_onstart");
    // the visitor's expired session ends before its replacement starts
    if (!g_sess.pendingEnd.empty()) {
        std::string old = std::move(g_sess.pendingEnd);
        g_sess.pendingEnd.clear();
        runSessionOnEnd(old);
    }
    if (ip.aspNewSession) ip.callGlobalProc("session_onstart");
}

bool aspResponseWrite(Interp& ip, const std::string& s) {
    if (!ip.aspMode || !ip.responseObj) return false;
    responseBody(s);
    return true;
}

void flushAspResponse(Interp& ip) {
    (void)ip;
    if (!g_resp) return;
    try {
        syncRespProps();
        flushBody();
    } catch (VBSError& e) {
        // the response cannot be built (e.g. an invalid header value): send
        // a clean 500 rather than nothing, and never the page body headerless
        std::fprintf(stderr, "mscript: ASP response error: %s\n", e.what());
        if (!g_resp->headersSent && std::getenv("REQUEST_METHOD")) {
            static const char err[] = "Status: 500 Internal Server Error\r\n"
                                      "Content-Type: text/plain\r\n\r\n"
                                      "Internal Server Error\n";
            std::fwrite(err, 1, sizeof err - 1, stdout);
            g_resp->headersSent = true;
        }
        g_resp->body.clear();
        std::fflush(stdout);
    }
}
