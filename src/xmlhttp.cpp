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

#include <cctype>
#include <algorithm>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr size_t MAX_RESPONSE_BYTES = 64 * 1024 * 1024;

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
    long long timeoutMs = 0; // 0 = no timeout
    bool aborted = false;
    std::vector<std::pair<std::string, std::string>> reqHeaders;
    HttpResponse resp;
    bool hasResponse = false;
    std::string charset = "utf-8";
    std::shared_ptr<VBObject> responseXml;
    bool responseXmlTried = false;
};

std::string headerValue(const std::vector<std::pair<std::string, std::string>>& hs, const std::string& name) {
    for (auto& h : hs)
        if (lower(h.first) == lower(name)) return h.second;
    return "";
}

bool isToken(const std::string& s) {
    if (s.empty()) return false;
    for (unsigned char c : s)
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9')) &&
            std::string("!#$%&'*+-.^_`|~").find(c) == std::string::npos)
            return false;
    return true;
}

bool hasControl(const std::string& s) {
    for (unsigned char c : s) if (c < 0x20 || c == 0x7f) return true;
    return false;
}

void validateUrl(const std::string& url) {
    // A URL becomes the HTTP request target (or a libfetch C string).
    if (url.empty() || hasControl(url) || url.find(' ') != std::string::npos)
        throwErr(5, "XMLHTTP: invalid URL");
}

// ---- URL helpers (parsing via libfetch) ----

struct UrlParts {
    std::string scheme, host, port, doc;
};

UrlParts parseUrl(const std::string& url) {
    validateUrl(url);
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
    if (p.scheme != "http" && p.scheme != "https" && p.scheme != "file")
        throwErr(5, "XMLHTTP: unsupported URL scheme");
    if ((p.scheme == "http" || p.scheme == "https") &&
        (p.host.empty() || hasControl(p.host)))
        throwErr(5, "XMLHTTP: invalid URL '" + url + "'");
    return p;
}

std::string resolveUrl(const std::string& base, const std::string& loc) {
    validateUrl(loc);
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

bool sameOrigin(const std::string& a, const std::string& b) {
    UrlParts x = parseUrl(a), y = parseUrl(b);
    return x.scheme == y.scheme && lower(x.host) == lower(y.host) && x.port == y.port;
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
        if (end == line.c_str() || *end != '\0' || sz < 0) return false;
        if (sz == 0) return true;
        i = e + 2;
        if ((size_t)sz > in.size() - i || in.size() - i - (size_t)sz < 2) return false;
        if (in[i + (size_t)sz] != '\r' || in[i + (size_t)sz + 1] != '\n') return false;
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
        validateUrl(ps);
        size_t s = ps.find("://");
        if (s != std::string::npos) ps = ps.substr(s + 3);
        size_t slash = ps.find('/');
        if (slash != std::string::npos) ps = ps.substr(0, slash);
        size_t colon = ps.rfind(':');
        if (colon != std::string::npos) { connectHost = ps.substr(0, colon); connectPort = ps.substr(colon + 1); }
        else connectHost = ps;
        if (connectPort.empty()) connectPort = "8080";
        if (connectHost.empty() || hasControl(connectHost) || hasControl(connectPort))
            throwErr(5, "XMLHTTP: invalid proxy");
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
        if ((size_t)n > MAX_RESPONSE_BYTES - raw.size()) {
            close(fd);
            throwErr(5, "XMLHTTP: response too large");
        }
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
            std::string next = resolveUrl(url, loc);
            // Never send credentials to a different origin after a redirect.
            auto nextHeaders = headers;
            if (!sameOrigin(url, next)) {
                nextHeaders.erase(std::remove_if(nextHeaders.begin(), nextHeaders.end(), [](const auto& h) {
                    std::string n = lower(h.first);
                    return n == "authorization" || n == "proxy-authorization" || n == "cookie";
                }), nextHeaders.end());
            }
            if (toGet) {
                nextHeaders.erase(std::remove_if(nextHeaders.begin(), nextHeaders.end(), [](const auto& h) {
                    return lower(h.first) == "content-type";
                }), nextHeaders.end());
            }
            return httpRaw(toGet ? "GET" : method, next, nextHeaders,
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
    struct TimeoutGuard {
        int old = fetchTimeout;
        ~TimeoutGuard() { fetchTimeout = old; }
    } timeoutGuard;
    if (timeoutMs > 0)
        fetchTimeout = (int)std::min(timeoutMs / 1000 + (timeoutMs % 1000 != 0), (long long)INT_MAX);
    struct url* u = fetchParseURL(url.c_str());
    if (!u) throwErr(5, "XMLHTTP: invalid URL '" + url + "'");
    if (user.size() >= sizeof u->user || password.size() >= sizeof u->pwd) {
        fetchFreeURL(u);
        throwErr(5, "XMLHTTP: credentials too long");
    }
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
    } else if (scheme == "https") {
        if (body.find('\0') != std::string::npos) {
            fetchFreeURL(u);
            throwErr(5, "XMLHTTP: libfetch cannot send a binary request body");
        }
        f = fetchReqHTTP(u, method.c_str(), "",
                         contentType.empty() ? nullptr : contentType.c_str(),
                         body.empty() ? nullptr : body.c_str());
    }
    fetchFreeURL(u);
    if (scheme != "file" && scheme != "https")
        throwErr(5, "XMLHTTP: unsupported URL scheme");

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
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) {
        if (n > MAX_RESPONSE_BYTES - out.body.size()) {
            fclose(f);
            throwErr(5, "XMLHTTP: response too large");
        }
        out.body.append(buf, n);
    }
    bool readError = ferror(f);
    fclose(f);
    if (readError) throwErr(5, "XMLHTTP: response read failed");
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
    explicit XmlHttpRequest(std::shared_ptr<XhrData> data) : d(std::move(data)) {
        kind = "MSXML2.XMLHTTP";
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

} // namespace

std::shared_ptr<VBObject> makeXmlHttp(Interp& interp) {
    auto data = std::make_shared<XhrData>();
    data->interp = &interp;
    auto o = std::make_shared<XmlHttpRequest>(data);

    o->methods["open"] = [data](MapObject*, std::vector<Variant>& a) -> Variant {
        if (a.size() < 2) throwErr(450, errDesc(450));
        std::string m = trim(toStr(a[0]));
        for (auto& ch : m) ch = (char)std::toupper((unsigned char)ch);
        if (!isToken(m)) throwErr(5, "XMLHTTP: invalid method");
        std::string url = trim(toStr(a[1]));
        parseUrl(url);
        data->method = m;
        data->url = url;
        data->user = a.size() > 3 ? toStr(a[3]) : "";
        data->password = a.size() > 4 ? toStr(a[4]) : "";
        data->reqHeaders.clear();
        // a[2] (async) is accepted but requests are always synchronous
        data->readyState = 1;
        data->aborted = false;
        data->hasResponse = false;
        data->resp = HttpResponse {};
        data->responseXml.reset();
        data->responseXmlTried = false;
        return Variant();
    };

    o->methods["send"] = [data](MapObject*, std::vector<Variant>& a) -> Variant {
        if (data->readyState != 1) throwErr(5, "XMLHTTP: open() must be called before send()");
        if (data->aborted) {
            data->aborted = false;
            data->readyState = 0;
            return Variant();
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
        UrlParts u = parseUrl(data->url);
        std::string ct = headerValue(data->reqHeaders, "content-type");
        HttpResponse r = (u.scheme == "http")
            ? httpRaw(data->method, data->url, data->reqHeaders, body, data->timeoutMs, 5)
            : httpViaLibfetch(data->method, data->url, ct, body, data->user, data->password, data->timeoutMs);
        data->resp = std::move(r);
        data->hasResponse = true;
        data->readyState = 4;
        data->charset = charsetFromContentType(data->resp.header("content-type"));
        data->responseXml.reset();
        data->responseXmlTried = false;
        return Variant();
    };

    o->methods["setrequestheader"] = [data](MapObject*, std::vector<Variant>& a) -> Variant {
        if (a.size() < 2) throwErr(450, errDesc(450));
        if (data->readyState != 1) throwErr(5, "XMLHTTP: setRequestHeader must be called after open()");
        std::string name = toStr(a[0]);
        std::string value = toStr(a[1]);
        std::string ln = lower(name);
        if (!isToken(name) || hasControl(value)) throwErr(5, "XMLHTTP: invalid header");
        // Framing and connection headers are generated by this client.
        if (ln == "host" || ln == "content-length" || ln == "transfer-encoding" ||
            ln == "connection" || ln == "proxy-connection")
            throwErr(5, "XMLHTTP: reserved header");
        for (auto& h : data->reqHeaders) {
            if (lower(h.first) == ln) { h.second = value; return Variant(); }
        }
        data->reqHeaders.emplace_back(name, value);
        return Variant();
    };

    o->methods["getallresponseheaders"] = [data](MapObject*, std::vector<Variant>&) -> Variant {
        if (!data->hasResponse) return Variant::mkStr("");
        std::string s;
        for (auto& h : data->resp.headers) s += h.first + ": " + h.second + "\r\n";
        return Variant::mkStr(s);
    };

    o->methods["getresponseheader"] = [data](MapObject*, std::vector<Variant>& a) -> Variant {
        if (a.empty()) throwErr(450, errDesc(450));
        if (!data->hasResponse) return Variant::mkStr("");
        return Variant::mkStr(data->resp.header(lower(toStr(a[0]))));
    };

    o->methods["abort"] = [data](MapObject*, std::vector<Variant>&) -> Variant {
        data->aborted = true;
        data->readyState = 0;
        data->hasResponse = false;
        data->resp = HttpResponse {};
        return Variant();
    };

    return o;
}
