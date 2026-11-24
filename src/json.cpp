// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 Lucas Holt. All rights reserved.

// json.cpp — JSON extension (not part of VBScript): a global JSON object with
// JavaScript-style Parse/Stringify methods. Parsing is done with libucl from
// the base system (libprivateucl; the ports libucl works too). When the ucl.h
// header is not installed (base installs only the library), the few C API
// entry points used here are declared directly — the API is stable.
#include "json.h"

#include "interpreter.h"
#include "objects.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <format>
#include <string>
#include <vector>

#if __has_include(<ucl.h>)
#include <ucl.h>
#else
extern "C" {
typedef enum ucl_type {
    UCL_OBJECT = 0, UCL_ARRAY, UCL_INT, UCL_FLOAT, UCL_STRING,
    UCL_BOOLEAN, UCL_TIME, UCL_USERDATA, UCL_NULL
} ucl_type_t;
typedef struct ucl_object_s ucl_object_t;
typedef void* ucl_object_iter_t;
struct ucl_parser* ucl_parser_new(int flags);
bool ucl_parser_add_string(struct ucl_parser* parser, const char* data, size_t len);
ucl_object_t* ucl_parser_get_object(struct ucl_parser* parser);
const char* ucl_parser_get_error(struct ucl_parser* parser);
void ucl_parser_free(struct ucl_parser* parser);
void ucl_object_unref(ucl_object_t* obj);
ucl_type_t ucl_object_type(const ucl_object_t* obj);
const char* ucl_object_tostring(const ucl_object_t* obj);
int64_t ucl_object_toint(const ucl_object_t* obj);
double ucl_object_todouble(const ucl_object_t* obj);
bool ucl_object_toboolean(const ucl_object_t* obj);
const char* ucl_object_key(const ucl_object_t* obj);
const ucl_object_t* ucl_object_iterate_with_error(const ucl_object_t* obj,
                                                   ucl_object_iter_t* iter, bool expand, int* ep);
}
#endif

namespace {

// extension errors use the VBScript user range (1000+); 1001 = JSON error
const int VB_E_JSON = 1001;

Variant uclToVariant(const ucl_object_t* o) {
    switch (ucl_object_type(o)) {
        case UCL_OBJECT: {
            auto m = std::make_shared<MapObject>();
            m->kind = "Object";
            ucl_object_iter_t it = nullptr;
            const ucl_object_t* cur;
            while ((cur = ucl_object_iterate_with_error(o, &it, true, nullptr)) != nullptr) {
                const char* k = ucl_object_key(cur);
                // VBObject member names are lowercased, so JSON keys are too
                // (VBScript property access is case-insensitive)
                m->props[lower(k ? k : "")] = uclToVariant(cur);
            }
            return Variant::mkObj(std::move(m));
        }
        case UCL_ARRAY: {
            std::vector<Variant> items;
            ucl_object_iter_t it = nullptr;
            const ucl_object_t* cur;
            while ((cur = ucl_object_iterate_with_error(o, &it, true, nullptr)) != nullptr)
                items.push_back(uclToVariant(cur));
            auto a = std::make_shared<VArray>();
            a->alloc({(long long)items.size()}, 0, false);
            for (size_t i = 0; i < items.size(); i++) a->data[i] = std::move(items[i]);
            return Variant::mkArr(std::move(a));
        }
        case UCL_STRING: return Variant::mkStr(ucl_object_tostring(o) ? ucl_object_tostring(o) : "");
        case UCL_INT:    return Variant::mkInt((long long)ucl_object_toint(o)); // mkInt picks I2/I4/R8
        case UCL_FLOAT:
        case UCL_TIME:   return Variant::mkDouble(ucl_object_todouble(o));
        case UCL_BOOLEAN: return Variant::mkBool(ucl_object_toboolean(o));
        case UCL_NULL:   return Variant::mkNull();
        default:         return Variant();
    }
}

void jsonEscape(std::string& out, const std::string& s) {
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", c);
                    out += buf;
                } else {
                    out += (char)c;
                }
        }
    }
    out += '"';
}

std::string jsonNumber(double d) {
    if (!std::isfinite(d)) return "null"; // JS: Infinity/NaN are not valid JSON
    return std::format("{}", d); // C++23: shortest round-trip form
}

// JS Date#toJSON: ISO 8601 UTC string from the VB date serial (1899-12-30 == 0)
std::string jsonDate(double serial) {
    double days = std::floor(serial);
    double frac = serial - days;
    long long unixDays = (long long)days - VB_EPOCH_OFF;
    long long y; unsigned m, d;
    civilFromDays(unixDays, y, m, d);
    long long totalMs = (long long)std::llround(frac * 86400000.0);
    long long secs = totalMs / 1000, ms = totalMs % 1000;
    if (secs >= 86400) { secs -= 86400; unixDays++; civilFromDays(unixDays, y, m, d); }
    long long h = secs / 3600, mi = (secs / 60) % 60, s = secs % 60;
    char buf[40];
    std::snprintf(buf, sizeof buf, "%04lld-%02u-%02uT%02lld:%02lld:%02lld.%03lldZ",
                  y, m, d, h, mi, s, ms);
    return std::string("\"") + buf + "\"";
}

struct Emitter {
    std::string indent; // per-level indentation (empty = compact)
    int depth = 0;
    static constexpr int MAX_DEPTH = 64;

    void newlineLevel(std::string& out, int level) const {
        if (!indent.empty()) {
            out += '\n';
            for (int i = 0; i < level; i++) out += indent;
        }
    }

    void emit(std::string& out, const Variant& v, int level, const std::vector<std::string>* onlyKeys) {
        if (++depth > MAX_DEPTH) throwErr(VB_E_JSON, "JSON.stringify: structure too deep or circular");
        struct Guard { int& d; ~Guard() { d--; } } g { depth };
        switch (v.t) {
            case VT::Empty:
            case VT::Null: out += "null"; break;
            case VT::Object:
                if (!v.obj) { out += "null"; break; } // Nothing
                if (auto* m = dynamic_cast<MapObject*>(v.obj.get())) {
                    out += '{';
                    bool first = true;
                    for (auto& kv : m->props) {
                        if (onlyKeys && !std::count(onlyKeys->begin(), onlyKeys->end(), kv.first)) continue;
                        if (!first) out += ',';
                        first = false;
                        newlineLevel(out, level + 1);
                        jsonEscape(out, kv.first);
                        out += ':';
                        if (!indent.empty()) out += ' ';
                        emit(out, kv.second, level + 1, nullptr);
                    }
                    if (!first) newlineLevel(out, level);
                    out += '}';
                } else if (auto* ci = dynamic_cast<ClassInstance*>(v.obj.get())) {
                    out += '{';
                    bool first = true;
                    for (auto& kv : ci->fields.vars) {
                        if (onlyKeys && !std::count(onlyKeys->begin(), onlyKeys->end(), kv.first)) continue;
                        if (!first) out += ',';
                        first = false;
                        newlineLevel(out, level + 1);
                        jsonEscape(out, kv.first);
                        out += ':';
                        if (!indent.empty()) out += ' ';
                        emit(out, kv.second, level + 1, nullptr);
                    }
                    if (!first) newlineLevel(out, level);
                    out += '}';
                } else {
                    out += "null"; // host objects are not serializable
                }
                break;
            case VT::Array:
                if (!v.arr) { out += "null"; break; }
                out += '[';
                if (v.arr->allocated) {
                    for (size_t i = 0; i < v.arr->data.size(); i++) {
                        if (i) out += ',';
                        newlineLevel(out, level + 1);
                        emit(out, v.arr->data[i], level + 1, nullptr);
                    }
                    if (!v.arr->data.empty()) newlineLevel(out, level);
                }
                out += ']';
                break;
            case VT::Bool:  out += v.b ? "true" : "false"; break;
            case VT::Int:
            case VT::Long:
            case VT::Byte:  out += std::to_string(v.ll); break;
            case VT::Currency: {
                double d = (double)v.ll / (double)CY_SCALE;
                out += jsonNumber(d);
                break;
            }
            case VT::Single:
            case VT::Double: out += jsonNumber(v.d); break;
            case VT::Date:   out += jsonDate(v.d); break;
            case VT::String: jsonEscape(out, v.s); break;
        }
    }
};

} // namespace

std::shared_ptr<VBObject> makeJson(Interp& ip) {
    (void)ip;
    auto j = std::make_shared<MapObject>();
    j->kind = "JSON";
    j->methods["parse"] = [](MapObject*, std::vector<Variant>& a) -> Variant {
        if (a.empty()) throwErr(450, errDesc(450));
        std::string s = toStr(a[0]);
        ucl_parser* p = ucl_parser_new(0);
        if (!p) throwErr(7, errDesc(7));
        if (!ucl_parser_add_string(p, s.data(), s.size())) {
            std::string msg = "JSON.parse: ";
            const char* e = ucl_parser_get_error(p);
            msg += e ? e : "invalid JSON";
            ucl_parser_free(p);
            throw VBSError(VB_E_JSON, msg);
        }
        ucl_object_t* top = ucl_parser_get_object(p);
        ucl_parser_free(p);
        if (!top) throw VBSError(VB_E_JSON, "JSON.parse: empty input");
        Variant v = uclToVariant(top);
        ucl_object_unref(top);
        return v;
    };
    j->methods["stringify"] = [](MapObject*, std::vector<Variant>& a) -> Variant {
        if (a.empty()) return Variant::mkStr(""); // JS: stringify() is undefined; Empty here
        Emitter em;
        std::vector<std::string> onlyKeys;
        const std::vector<std::string>* keyFilter = nullptr;
        if (a.size() >= 2 && a[1].t == VT::Array && a[1].arr) {
            for (auto& e : a[1].arr->data) onlyKeys.push_back(lower(toStr(e)));
            keyFilter = &onlyKeys;
        }
        if (a.size() >= 3) {
            if (a[2].t == VT::String) {
                std::string s = toStr(a[2]);
                if (s.size() > 10) s.resize(10);
                em.indent = s;
            } else {
                long long n = toInt64(a[2]);
                if (n < 0) n = 0;
                if (n > 10) n = 10;
                em.indent.assign((size_t)n, ' ');
            }
        }
        std::string out;
        em.emit(out, a[0], 0, keyFilter);
        return Variant::mkStr(std::move(out));
    };
    return j;
}
