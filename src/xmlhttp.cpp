// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 Lucas Holt. All rights reserved.

// xmlhttp.cpp — MSXML2.XMLHTTP-compatible object.
//   http://   -> built-in HTTP/1.1 client over POSIX sockets
//                (full status line, response headers, chunked decoding, redirects,
//                 custom request headers from setRequestHeader)
//   https:// -> libfetch fetchReqHTTP (TLS); response headers are not exposed by
//                this libfetch API, so getResponseHeader returns "" there
//   file://   -> libfetch fetchGetURL
// Requests are always performed synchronously; the async flag of open() is
// accepted but ignored.

#include "xmlhttp.h"
#include "interpreter.h"
#include "xmldom.h"

#include <sys/param.h> // MAXHOSTNAMELEN, required by fetch.h
#include <fetch.h>

#include <netdb.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace {

struct HttpResponse {
    int status = 0;
    std::string statusText;
    std::vector<std::pair<std::string, std::string>> headers; // name kept in original case
    std::string body;

    std::string header(const std::string& lowerName) const {
        for (auto& h : headers)
            if (lower(h.first) == lowerName) return h.second;
        return "";
    }
};

struct XhrData {
    Interp* interp = nullptr;
    int readyState = 0; // 0 unopened, 1 opened, 4 done
    std::string method = "GET";
    std::string url;
    std::string user, password;
    long long timeoutMs = 0; // 0 = no timeout (XMLHTTP timeout property)
    bool aborted = false;
    std::vector<std::pair<std::string, std::string>> reqHeaders;
    HttpResponse resp;
    bool hasResponse = false;
    std::string charset = "utf-8";
    std::shared_ptr<VBObject> responseXml;
    bool responseXmlTried = false;
    // WinHttp.WinHttpRequest additions
    bool isWinHttp = false;
    std::string userAgent = "mscript/1.0";   // Option(0)
    bool enableRedirects = true;             // Option(6), default true in WinHTTP
    long long maxRedirects = 10;             // Option(14)
    long long tResolve = 0, tConnect = 0, tSend = 0, tReceive = 0; // SetTimeouts (ms)
    std::map<std::string, Variant> extraOptions; // other Option values, stored/ignored
};

std::string headerValue(const std::vector<std::pair<std::string, std::string>>& hs, const std::string& name) {
    for (auto& h : hs)
        if (lower(h.first) == lower(name)) return h.second;
    return "";
}

// ---- URL helpers (parsing via libfetch) ----

struct UrlParts {
    std::string scheme, host, port, doc;
};

UrlParts parseUrl(const std::string& url) {
    UrlParts p;
    struct url* u = fetchParseURL(url.c_str());
    if (!u) throwErr(5, "XMLHTTP: invalid URL '" + url + "'");
    p.scheme = lower(u->scheme);
    p.host = u->host;
    int port = u->port;
    if (port == 0) port = (p.scheme == "https") ? 443 : 80;
    p.port = std::to_string(port);
    p.doc = (u->doc && *u->doc) ? u->doc : "/";
    fetchFreeURL(u);
    if ((p.scheme == "http" || p.scheme == "https") && p.host.empty())
        throwErr(5, "XMLHTTP: invalid URL '" + url + "'");
    return p;
}

std::string resolveUrl(const std::string& base, const std::string& loc) {
    if (loc.rfind("http://", 0) == 0 || loc.rfind("https://", 0) == 0) return loc;
    UrlParts b = parseUrl(base);
    std::string origin = b.scheme + "://" + b.host;
    bool defPort = (b.scheme == "http" && b.port == "80") || (b.scheme == "https" && b.port == "443");
    if (!defPort) origin += ":" + b.port;
    if (!loc.empty() && loc[0] == '/') return origin + loc;
    std::string dir = b.doc;
    size_t slash = dir.rfind('/');
    if (slash != std::string::npos) dir = dir.substr(0, slash + 1); else dir = "/";
    return origin + dir + loc;
}

// ---- raw HTTP/1.1 over POSIX sockets (http://) ----

bool dechunk(const std::string& in, std::string& out) {
    size_t i = 0;
    while (true) {
        size_t e = in.find("\r\n", i);
        if (e == std::string::npos) return false;
        std::string line = in.substr(i, e - i);
        size_t semi = line.find(';');
        if (semi != std::string::npos) line = line.substr(0, semi);
        char* end = nullptr;
        long sz = std::strtol(line.c_str(), &end, 16);
        if (end == line.c_str() || sz < 0) return false;
        if (sz == 0) return true;
        i = e + 2;
        if (i + (size_t)sz + 2 > in.size()) return false;
        out.append(in, i, (size_t)sz);
        i += (size_t)sz + 2; // data + CRLF
    }
}

HttpResponse httpRaw(const std::string& method, const std::string& url,
                     const std::vector<std::pair<std::string, std::string>>& headers,
                     const std::string& body, long long timeoutMs, int redirectsLeft) {
    UrlParts u = parseUrl(url);
    if (u.scheme != "http") throwErr(5, "XMLHTTP: unsupported scheme '" + u.scheme + "'");

    // honor http_proxy / HTTP_PROXY (connect to the proxy, send absolute URI)
    std::string connectHost = u.host, connectPort = u.port, target = u.doc;
    const char* proxy = std::getenv("http_proxy");
    if (!proxy) proxy = std::getenv("HTTP_PROXY");
    if (proxy && *proxy) {
        std::string ps = proxy;
        size_t s = ps.find("://");
        if (s != std::string::npos) ps = ps.substr(s + 3);
        size_t slash = ps.find('/');
        if (slash != std::string::npos) ps = ps.substr(0, slash);
        size_t colon = ps.rfind(':');
        if (colon != std::string::npos) { connectHost = ps.substr(0, colon); connectPort = ps.substr(colon + 1); }
        else connectHost = ps;
        if (connectPort.empty()) connectPort = "8080";
        target = url; // absolute URI for proxies
    }

    struct addrinfo hints {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo* res = nullptr;
    if (getaddrinfo(connectHost.c_str(), connectPort.c_str(), &hints, &res) != 0 || !res)
        throwErr(5, "XMLHTTP: cannot resolve host '" + connectHost + "'");
    int fd = -1;
    for (struct addrinfo* ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        if (timeoutMs > 0) {
            timeval tv {};
            tv.tv_sec = timeoutMs / 1000;
            tv.tv_usec = (timeoutMs % 1000) * 1000;
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
            setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
        }
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) throwErr(5, "XMLHTTP: cannot connect to " + connectHost + ":" + connectPort);

    bool defPort = (u.port == "80");
    std::string req;
    req += method + " " + target + " HTTP/1.1\r\n";
    req += "Host: " + u.host + (defPort ? "" : ":" + u.port) + "\r\n";
    if (headerValue(headers, "accept").empty()) req += "Accept: */*\r\n";
    if (headerValue(headers, "user-agent").empty()) req += "User-Agent: mscript/1.0\r\n";
    bool hasCL = false;
    for (auto& h : headers) {
        req += h.first + ": " + h.second + "\r\n";
        if (lower(h.first) == "content-length") hasCL = true;
    }
    if (!body.empty() && !hasCL) req += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    req += "Connection: close\r\n\r\n";
    req += body;

    size_t sent = 0;
    while (sent < req.size()) {
        ssize_t n = send(fd, req.data() + sent, req.size() - sent, 0);
        if (n <= 0) { close(fd); throwErr(5, "XMLHTTP: send failed"); }
        sent += (size_t)n;
    }
    std::string raw;
    char buf[65536];
    while (true) {
        ssize_t n = recv(fd, buf, sizeof buf, 0);
        if (n < 0) { close(fd); throwErr(5, "XMLHTTP: receive failed (timeout or connection error)"); }
        if (n == 0) break;
        raw.append(buf, (size_t)n);
    }
    close(fd);

    HttpResponse out;
    size_t he = raw.find("\r\n\r\n");
    if (he == std::string::npos) throwErr(5, "XMLHTTP: malformed HTTP response");
    std::string head = raw.substr(0, he);
    std::string rbody = raw.substr(he + 4);

    size_t eol = head.find("\r\n");
    std::string statusLine = (eol == std::string::npos) ? head : head.substr(0, eol);
    size_t sp1 = statusLine.find(' ');
    if (sp1 == std::string::npos) throwErr(5, "XMLHTTP: malformed status line");
    out.status = std::atoi(statusLine.c_str() + sp1 + 1);
    size_t sp2 = statusLine.find(' ', sp1 + 1);
    out.statusText = (sp2 == std::string::npos) ? "" : statusLine.substr(sp2 + 1);

    size_t pos = (eol == std::string::npos) ? head.size() : eol + 2;
    while (pos < head.size()) {
        size_t le = head.find("\r\n", pos);
        if (le == std::string::npos) le = head.size();
        std::string line = head.substr(pos, le - pos);
        pos = le + 2;
        size_t c = line.find(':');
        if (c == std::string::npos) continue;
        out.headers.emplace_back(trim(line.substr(0, c)), trim(line.substr(c + 1)));
    }

    std::string te = out.header("transfer-encoding");
    if (te.find("chunked") != std::string::npos) {
        std::string dec;
        if (!dechunk(rbody, dec)) throwErr(5, "XMLHTTP: malformed chunked body");
        rbody = std::move(dec);
    } else {
        std::string cl = out.header("content-length");
        if (!cl.empty()) {
            long long n = std::atoll(cl.c_str());
            if (n >= 0 && (size_t)n < rbody.size()) rbody.resize((size_t)n);
        }
    }
    out.body = std::move(rbody);

    if ((out.status == 301 || out.status == 302 || out.status == 303 ||
         out.status == 307 || out.status == 308) && redirectsLeft > 0) {
        std::string loc = out.header("location");
        if (!loc.empty()) {
            bool toGet = (out.status == 303);
            return httpRaw(toGet ? "GET" : method, resolveUrl(url, loc), headers,
                           toGet ? "" : body, timeoutMs, redirectsLeft - 1);
        }
    }
    return out;
}

// ---- libfetch path (https://, file://) ----

int reasonToStatus(const std::string& reason) {
    std::string r = lower(reason);
    if (r.find("not found") != std::string::npos) return 404;
    if (r.find("unauthorized") != std::string::npos) return 401;
    if (r.find("forbidden") != std::string::npos) return 403;
    if (r.find("bad request") != std::string::npos) return 400;
    if (r.find("method not allowed") != std::string::npos) return 405;
    if (r.find("timeout") != std::string::npos) return 408;
    if (r.find("moved") != std::string::npos || r.find("found") != std::string::npos) return 302;
    if (r.find("not modified") != std::string::npos) return 304;
    if (r.find("internal") != std::string::npos) return 500;
    if (r.find("bad gateway") != std::string::npos) return 502;
    if (r.find("unavailable") != std::string::npos) return 503;
    if (r.find("gateway timeout") != std::string::npos) return 504;
    return 500;
}

HttpResponse httpViaLibfetch(const std::string& method, const std::string& url,
                             const std::string& contentType, const std::string& body,
                             const std::string& user, const std::string& password,
                             long long timeoutMs) {
    if (timeoutMs > 0) fetchTimeout = (int)(timeoutMs / 1000);
    struct url* u = fetchParseURL(url.c_str());
    if (!u) throwErr(5, "XMLHTTP: invalid URL '" + url + "'");
    if (!user.empty()) {
        std::strncpy(u->user, user.c_str(), sizeof u->user - 1);
        u->user[sizeof u->user - 1] = '\0';
    }
    if (!password.empty()) {
        std::strncpy(u->pwd, password.c_str(), sizeof u->pwd - 1);
        u->pwd[sizeof u->pwd - 1] = '\0';
    }
    std::string scheme = lower(u->scheme);
    FILE* f = nullptr;
    if (scheme == "file") {
        f = fetchGetFile(u, "");
    } else {
        f = fetchReqHTTP(u, method.c_str(), "",
                         contentType.empty() ? nullptr : contentType.c_str(),
                         body.empty() ? nullptr : body.c_str());
    }
    fetchFreeURL(u);

    HttpResponse out;
    if (!f) {
        int e = fetchLastErrCode;
        std::string es = fetchLastErrString;
        if (scheme == "file" || e == FETCH_UNAVAIL || e == FETCH_SERVER) {
            // HTTP-level error (or missing file): expose status, do not throw —
            // MSXML send() returns normally and reports the status code.
            out.status = (scheme == "file") ? 404 : reasonToStatus(es);
            out.statusText = es;
            return out;
        }
        throwErr(5, "XMLHTTP network error: " + es);
    }
    out.status = 200;
    out.statusText = "OK";
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) out.body.append(buf, n);
    fclose(f);
    return out;
}

// ---- charset handling for responseText ----

std::string charsetFromContentType(const std::string& ct) {
    std::string low = lower(ct);
    size_t p = low.find("charset=");
    if (p == std::string::npos) return "utf-8";
    std::string cs = trim(low.substr(p + 8));
    if (!cs.empty() && (cs[0] == '"' || cs[0] == '\'')) cs = cs.substr(1);
    size_t end = cs.find_first_of(";\"' \t\r\n");
    if (end != std::string::npos) cs = cs.substr(0, end);
    if (cs == "utf8") return "utf-8";
    if (cs == "ascii" || cs == "us-ascii") return "us-ascii";
    if (cs == "latin1" || cs == "iso-8859-1" || cs == "iso8859-1" || cs == "windows-1252") return "iso-8859-1";
    return "utf-8";
}

std::string decodeBody(const std::string& bytes, const std::string& charset) {
    if (charset == "iso-8859-1") {
        std::string out;
        out.reserve(bytes.size() * 2);
        for (unsigned char c : bytes) {
            if (c < 0x80) out += (char)c;
            else {
                out += (char)(0xC0 | (c >> 6));
                out += (char)(0x80 | (c & 0x3F));
            }
        }
        return out;
    }
    return bytes; // utf-8 / us-ascii pass through
}

// ---- the object ----

struct XmlHttpRequest : MapObject {
    std::shared_ptr<XhrData> d;
    explicit XmlHttpRequest(std::shared_ptr<XhrData> data, bool winhttp = false) : d(std::move(data)) {
        d->isWinHttp = winhttp;
        kind = winhttp ? "WinHttp.WinHttpRequest.5.1" : "MSXML2.XMLHTTP";
    }

    Variant getMember(const std::string& n) override {
        if (n == "readystate") return Variant::mkInt(d->readyState);
        if (n == "status") return Variant::mkLong(d->hasResponse ? d->resp.status : 0);
        if (n == "statustext") return Variant::mkStr(d->hasResponse ? d->resp.statusText : "");
        if (n == "responsetext")
            return Variant::mkStr(d->hasResponse ? decodeBody(d->resp.body, d->charset) : "");
        if (n == "responsebody") {
            if (!d->hasResponse) return Variant::mkNull();
            auto arr = std::make_shared<VArray>();
            arr->base = 0;
            arr->dims = { (long long)d->resp.body.size() };
            arr->data.resize(d->resp.body.size());
            for (size_t i = 0; i < d->resp.body.size(); i++)
                arr->data[i] = Variant::mkInt((unsigned char)d->resp.body[i]);
            return Variant::mkArr(arr);
        }
        if (n == "responsexml") {
            if (!d->hasResponse) return Variant::mkNull();
            if (!d->responseXmlTried) {
                d->responseXmlTried = true;
                std::string ct = lower(d->resp.header("content-type"));
                std::string t = trim(d->resp.body);
                bool looksXml = ct.find("xml") != std::string::npos || (!t.empty() && t[0] == '<');
                if (looksXml) {
                    bool ok = false;
                    auto doc = makeDomDocumentFromXML(*d->interp, d->resp.body, ok);
                    if (ok) d->responseXml = doc;
                }
            }
            return d->responseXml ? Variant::mkObj(d->responseXml) : Variant::mkNull();
        }
        if (n == "timeout") return Variant::mkLong(d->timeoutMs);
        return MapObject::getMember(n);
    }

    void setMember(const std::string& n, const Variant& v) override {
        if (n == "timeout") {
            long long t = toInt64(v);
            if (t < 0) throwErr(5, errDesc(5));
            d->timeoutMs = t;
            return;
        }
        // onreadystatechange and any other assignments are stored but ignored
        MapObject::setMember(n, v);
    }
};

// ---- shared method implementations (MSXML2.XMLHTTP and WinHttp.WinHttpRequest) ----

static void xhrOpen(const std::shared_ptr<XhrData>& data, std::vector<Variant>& a, bool winhttp) {
    if (a.size() < 2) throwErr(450, errDesc(450));
    std::string m = trim(toStr(a[0]));
    for (auto& ch : m) ch = (char)std::toupper((unsigned char)ch);
    data->method = m;
    data->url = trim(toStr(a[1]));
    if (winhttp) {
        // a[2] (async) is accepted but requests are always synchronous
        if (a.size() > 2) data->extraOptions["async"] = Variant::mkBool(toBool(a[2]));
    } else {
        if (a.size() > 3) data->user = toStr(a[3]);
        if (a.size() > 4) data->password = toStr(a[4]);
    }
    data->readyState = 1;
    data->aborted = false;
    data->hasResponse = false;
    data->resp = HttpResponse {};
    data->responseXml.reset();
    data->responseXmlTried = false;
}

static void xhrSend(const std::shared_ptr<XhrData>& data, std::vector<Variant>& a) {
    const char* who = data->isWinHttp ? "WinHttpRequest" : "XMLHTTP";
    if (data->readyState != 1)
        throwErr(5, std::string(who) + ": open() must be called before send()");
    if (data->aborted) {
        data->aborted = false;
        data->readyState = 0;
        return;
    }
    std::string body;
    if (!a.empty()) {
        const Variant& c = a[0];
        if (c.t == VT::Array && c.arr) {
            for (auto& e : c.arr->data) {
                long long b = toInt64(e);
                if (b < 0 || b > 255) throwErr(5, errDesc(5));
                body += (char)b;
            }
        } else if (c.t != VT::Empty && c.t != VT::Null) {
            body = toStr(c); // internal strings are utf-8
        }
    }
    if (data->isWinHttp) {
        // Option(0) user agent overrides any User-Agent request header
        bool found = false;
        for (auto& h : data->reqHeaders)
            if (lower(h.first) == "user-agent") { h.second = data->userAgent; found = true; }
        if (!found) data->reqHeaders.emplace_back("User-Agent", data->userAgent);
    }
    long long effTimeout = data->timeoutMs;
    if (effTimeout <= 0) effTimeout = std::max(data->tSend, data->tReceive);
    int redirects = data->enableRedirects
        ? (int)std::min<long long>(data->maxRedirects > 0 ? data->maxRedirects : 10, 50)
        : 0;
    UrlParts u = parseUrl(data->url);
    std::string ct = headerValue(data->reqHeaders, "content-type");
    HttpResponse r = (u.scheme == "http")
        ? httpRaw(data->method, data->url, data->reqHeaders, body, effTimeout, redirects)
        : httpViaLibfetch(data->method, data->url, ct, body, data->user, data->password, effTimeout);
    data->resp = std::move(r);
    data->hasResponse = true;
    data->readyState = 4;
    data->charset = charsetFromContentType(data->resp.header("content-type"));
    data->responseXml.reset();
    data->responseXmlTried = false;
}

static void xhrSetRequestHeader(const std::shared_ptr<XhrData>& data, std::vector<Variant>& a) {
    if (a.size() < 2) throwErr(450, errDesc(450));
    if (data->readyState != 1)
        throwErr(5, "setRequestHeader must be called after open() and before send()");
    std::string name = toStr(a[0]);
    std::string value = toStr(a[1]);
    std::string ln = lower(name);
    for (auto& h : data->reqHeaders) {
        if (lower(h.first) == ln) { h.second = value; return; }
    }
    data->reqHeaders.emplace_back(name, value);
}

static Variant xhrGetAllResponseHeaders(const std::shared_ptr<XhrData>& data) {
    if (!data->hasResponse) return Variant::mkStr("");
    std::string s;
    for (auto& h : data->resp.headers) s += h.first + ": " + h.second + "\r\n";
    return Variant::mkStr(s);
}

static Variant xhrGetResponseHeader(const std::shared_ptr<XhrData>& data, std::vector<Variant>& a) {
    if (a.empty()) throwErr(450, errDesc(450));
    if (!data->hasResponse) return Variant::mkStr("");
    return Variant::mkStr(data->resp.header(lower(toStr(a[0]))));
}

static void xhrAbort(const std::shared_ptr<XhrData>& data) {
    data->aborted = true;
    data->readyState = 0;
    data->hasResponse = false;
    data->resp = HttpResponse {};
}

// ---- WinHttp.WinHttpRequest Option property (parameterized: x.Option(n) [/= v]) ----
// WinHttpRequestOption values (winhttp.h): 0 UserAgentString, 1 URL (read-only),
// 6 EnableRedirects, 14 MaxAutomaticRedirects, others accepted and stored.

static Variant xhrGetOption(XhrData& d, long long n) {
    switch (n) {
    case 0: return Variant::mkStr(d.userAgent);
    case 1: return Variant::mkStr(d.url);
    case 6: return Variant::mkBool(d.enableRedirects);
    case 14: return Variant::mkLong(d.maxRedirects);
    default: {
        auto it = d.extraOptions.find(std::to_string(n));
        return it != d.extraOptions.end() ? it->second : Variant();
    }
    }
}

static void xhrSetOption(XhrData& d, long long n, const Variant& v) {
    switch (n) {
    case 0: d.userAgent = toStr(v); return;
    case 1: throwErr(5, "WinHttpRequestOption_URL (Option(1)) is read-only"); return;
    case 6: d.enableRedirects = toBool(v); return;
    case 14: {
        long long m = toInt64(v);
        if (m < 0) throwErr(5, errDesc(5));
        d.maxRedirects = m;
        return;
    }
    default: d.extraOptions[std::to_string(n)] = v; return;
    }
}

struct OptionProxy : VBObject {
    std::shared_ptr<XhrData> d;
    explicit OptionProxy(std::shared_ptr<XhrData> data) : d(std::move(data)) {}
    Variant invoke(const std::vector<Variant>& args) override { // x.Option(n)
        if (args.empty()) throwErr(450, errDesc(450));
        return xhrGetOption(*d, toInt64(args[0]));
    }
    void assignInvoke(const std::vector<Variant>& args, const Variant& v) override { // x.Option(n) = v
        if (args.empty()) throwErr(450, errDesc(450));
        xhrSetOption(*d, toInt64(args[0]), v);
    }
    std::string objectKind() const override { return "WinHttpRequestOption"; }
};

struct WinHttpRequest : XmlHttpRequest {
    explicit WinHttpRequest(std::shared_ptr<XhrData> data) : XmlHttpRequest(data, true) {}
    bool hasMember(const std::string& n) const override {
        return n == "option" || XmlHttpRequest::hasMember(n);
    }
    Variant getMember(const std::string& n) override {
        if (n == "option") return Variant::mkObj(std::make_shared<OptionProxy>(d));
        return XmlHttpRequest::getMember(n);
    }
};

} // namespace

std::shared_ptr<VBObject> makeXmlHttp(Interp& interp) {
    auto data = std::make_shared<XhrData>();
    data->interp = &interp;
    auto o = std::make_shared<XmlHttpRequest>(data);

    o->methods["open"] = [data](MapObject*, std::vector<Variant>& a) -> Variant {
        xhrOpen(data, a, false);
        return Variant();
    };
    o->methods["send"] = [data](MapObject*, std::vector<Variant>& a) -> Variant {
        xhrSend(data, a);
        return Variant();
    };
    o->methods["setrequestheader"] = [data](MapObject*, std::vector<Variant>& a) -> Variant {
        xhrSetRequestHeader(data, a);
        return Variant();
    };
    o->methods["getallresponseheaders"] = [data](MapObject*, std::vector<Variant>&) -> Variant {
        return xhrGetAllResponseHeaders(data);
    };
    o->methods["getresponseheader"] = [data](MapObject*, std::vector<Variant>& a) -> Variant {
        return xhrGetResponseHeader(data, a);
    };
    o->methods["abort"] = [data](MapObject*, std::vector<Variant>&) -> Variant {
        xhrAbort(data);
        return Variant();
    };
    return o;
}

std::shared_ptr<VBObject> makeWinHttpRequest(Interp& interp) {
    auto data = std::make_shared<XhrData>();
    data->interp = &interp;
    auto o = std::make_shared<WinHttpRequest>(data);

    o->methods["open"] = [data](MapObject*, std::vector<Variant>& a) -> Variant {
        xhrOpen(data, a, true);
        return Variant();
    };
    o->methods["send"] = [data](MapObject*, std::vector<Variant>& a) -> Variant {
        xhrSend(data, a);
        return Variant();
    };
    o->methods["setrequestheader"] = [data](MapObject*, std::vector<Variant>& a) -> Variant {
        xhrSetRequestHeader(data, a);
        return Variant();
    };
    o->methods["getallresponseheaders"] = [data](MapObject*, std::vector<Variant>&) -> Variant {
        return xhrGetAllResponseHeaders(data);
    };
    o->methods["getresponseheader"] = [data](MapObject*, std::vector<Variant>& a) -> Variant {
        return xhrGetResponseHeader(data, a);
    };
    o->methods["abort"] = [data](MapObject*, std::vector<Variant>&) -> Variant {
        xhrAbort(data);
        return Variant();
    };
    o->methods["waitforresponse"] = [](MapObject*, std::vector<Variant>&) -> Variant {
        return Variant::mkBool(true); // requests are always synchronous
    };
    o->methods["settimeouts"] = [data](MapObject*, std::vector<Variant>& a) -> Variant {
        if (a.size() < 4) throwErr(450, errDesc(450));
        data->tResolve = toInt64(a[0]);
        data->tConnect = toInt64(a[1]);
        data->tSend = toInt64(a[2]);
        data->tReceive = toInt64(a[3]);
        return Variant();
    };
    o->methods["setcredentials"] = [data](MapObject*, std::vector<Variant>& a) -> Variant {
        if (a.size() < 2) throwErr(450, errDesc(450));
        data->user = toStr(a[0]);
        data->password = toStr(a[1]); // a[2] (flags) accepted, ignored
        return Variant();
    };
    // Proxies are honored via http_proxy/HTTP_PROXY env (and libfetch env vars);
    // SetProxy/SetClientCertificate are accepted as no-ops.
    o->methods["setproxy"] = [](MapObject*, std::vector<Variant>&) -> Variant { return Variant(); };
    o->methods["setclientcertificate"] = [](MapObject*, std::vector<Variant>&) -> Variant { return Variant(); };
    return o;
}
