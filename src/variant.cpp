// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 Lucas Holt. All rights reserved.

#include "variant.h"

#include <algorithm>
#include <cctype>
#include <climits>
#include <cmath>
#include <cstdint>
#include <limits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>

// Error descriptions, throwErr() and mapHRESULTToVB() live in errtable.cpp.

// ---------- VBObject ----------

Variant VBObject::getMember(const std::string&) {
    throwErr(438, errDesc(438));
}
void VBObject::setMember(const std::string&, const Variant&) {
    throwErr(438, errDesc(438));
}
Variant VBObject::callMember(const std::string&, std::vector<Variant>&) {
    throwErr(438, errDesc(438));
}
Variant VBObject::invoke(const std::vector<Variant>&) {
    throwErr(438, errDesc(438));
}
void VBObject::assignInvoke(const std::vector<Variant>&, const Variant&) {
    throwErr(438, errDesc(438));
}
std::vector<Variant> VBObject::enumItems() {
    throwErr(451, errDesc(451));
}
std::string VBObject::toString() { return objectKind(); }

Variant MapObject::getMember(const std::string& n) {
    auto it = props.find(n);
    if (it != props.end()) return it->second;
    auto mit = methods.find(n);
    if (mit != methods.end()) {
        // parameterless method used as property (e.g. dict.Keys)
        std::vector<Variant> noargs;
        return mit->second(this, noargs);
    }
    throwErr(438, errDesc(438));
}
void MapObject::setMember(const std::string& n, const Variant& v) {
    Variant& slot = props[n];
    slot = v;
    ownArrays(slot);
}
Variant MapObject::callMember(const std::string& n, std::vector<Variant>& args) {
    auto it = methods.find(n);
    if (it == methods.end()) {
        if (props.count(n)) throwErr(424, errDesc(424)); // property used as method
        throwErr(438, errDesc(438));
    }
    return it->second(this, args);
}
Variant MapObject::invoke(const std::vector<Variant>& args) {
    if (onInvoke) {
        std::vector<Variant> a = args;
        return onInvoke(this, a);
    }
    throwErr(438, errDesc(438));
}
void MapObject::assignInvoke(const std::vector<Variant>& args, const Variant& v) {
    if (onAssignInvoke) {
        std::vector<Variant> a = args;
        onAssignInvoke(this, a, v);
        return;
    }
    throwErr(438, errDesc(438));
}
std::vector<Variant> MapObject::enumItems() {
    if (onEnum) return onEnum(this);
    throwErr(451, errDesc(451));
}

// ---------- string helpers ----------

std::string lower(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) a++;
    while (b > a && std::isspace((unsigned char)s[b - 1])) b--;
    return s.substr(a, b - a);
}

// ---------- date algorithms (Howard Hinnant, proleptic Gregorian) ----------

long long daysFromCivil(long long y, unsigned m, unsigned d) {
    y -= m <= 2;
    const long long era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (long long)doe - 719468;
}

void civilFromDays(long long z, long long& y, unsigned& m, unsigned& d) {
    z += 719468;
    const long long era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = (unsigned)(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    y = (long long)yoe + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp + (mp < 10 ? 3 : -9);
    y += (m <= 2);
}

unsigned daysInMonth(long long y, unsigned m) {
    static const unsigned dm[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    if (m == 2) {
        bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
        return leap ? 29 : 28;
    }
    return dm[m - 1];
}

double makeSerial(long long y, unsigned m, unsigned d, int hh, int mm, int ss) {
    long long days = daysFromCivil(y, m, d) + VB_EPOCH_OFF; // VB serial: 1899-12-30 == 0
    return (double)days + (hh * 3600.0 + mm * 60.0 + ss) / 86400.0;
}

int weekdayOf(double serial) {
    long long days = (long long)std::floor(serial);
    long long wd = ((days % 7) + 7) % 7; // 0 == Saturday (1899-12-30)
    if (wd == 0) wd = 7;                  // 7 == Saturday
    // map: 0->Sat(7), 1->Sun(1), 2->Mon(2) ...
    if (wd == 7) return 7;
    return (int)wd; // 1..6 => Sun..Fri
}

static bool parseOneDate(const std::string& tok, double& out) {
    // Accept: M/D/Y, M/D/YY, YYYY-M-D, M-D-Y with optional time "h:mm[:ss] [AM|PM]"
    std::string t = trim(tok);
    if (t.empty()) return false;
    // split off time part
    std::string datePart = t, timePart;
    size_t sp = t.find_first_of(" \t");
    bool hasAMPM = false;
    std::string ampm;
    if (sp != std::string::npos) {
        datePart = t.substr(0, sp);
        std::string rest = trim(t.substr(sp + 1));
        // check trailing AM/PM
        if (rest.size() >= 2) {
            std::string tail = lower(rest.substr(rest.size() - 2));
            if (tail == "am" || tail == "pm") {
                hasAMPM = true;
                ampm = tail;
                rest = trim(rest.substr(0, rest.size() - 2));
            }
        }
        if (!rest.empty()) timePart = rest;
    }
    long long y; unsigned m, d;
    char sep = 0;
    auto parse3 = [&](long long a, long long b, long long c) -> bool {
        // M/D/Y or Y-M-D
        if (a > 31 && sep != 0) { y = a; m = (unsigned)b; d = (unsigned)c; }
        else if (a >= 1 && a <= 12 && b >= 1 && b <= 31) { m = (unsigned)a; d = (unsigned)b; y = c; }
        else return false;
        if (y < 100) y += (y < 30 ? 2000 : 1900);
        if (m < 1 || m > 12 || d < 1 || d > daysInMonth(y, m)) return false;
        return true;
    };
    long long p1 = 0, p2 = 0, p3 = 0;
    if (datePart.find('-') != std::string::npos) sep = '-';
    else if (datePart.find('/') != std::string::npos) sep = '/';
    else if (datePart.find(':') != std::string::npos) {
        // time-only string: "2:30 PM"
        y = 1899; m = 12; d = 30;
        if (!timePart.empty()) {
            // move time from datePart into timePart
            timePart = datePart + (timePart.empty() ? "" : " " + timePart);
            datePart.clear();
        }
        // parse the time from datePart (timePart holds any trailing extra)
        std::string tp = datePart + (timePart.empty() ? "" : " " + timePart);
        std::vector<std::string> tpv;
        size_t st = 0;
        for (size_t i = 0; i <= tp.size(); i++) {
            if (i == tp.size() || tp[i] == ':') { tpv.push_back(tp.substr(st, i - st)); st = i + 1; }
        }
        if (tpv.size() < 2 || tpv.size() > 3) return false;
        for (auto& p : tpv) if (p.empty() || !std::all_of(p.begin(), p.end(), [](char c){ return std::isdigit((unsigned char)c); })) return false;
        int hh = std::atoi(tpv[0].c_str()), mm = std::atoi(tpv[1].c_str()), ss = tpv.size() == 3 ? std::atoi(tpv[2].c_str()) : 0;
        if (hasAMPM) {
            if (ampm == "pm" && hh < 12) hh += 12;
            if (ampm == "am" && hh == 12) hh = 0;
        }
        if (hh > 23 || mm > 59 || ss > 59) return false;
        out = makeSerial(y, m, d, hh, mm, ss);
        return true;
    } else return false;
    if (std::sscanf(datePart.c_str(), "%lld%c%lld%c%lld", &p1, &sep, &p2, &sep, &p3) != 5) {
        // sscanf with %c re-reads; do manual split instead
    }
    // manual split on sep
    std::vector<std::string> parts;
    size_t start = 0;
    for (size_t i = 0; i <= datePart.size(); i++) {
        if (i == datePart.size() || datePart[i] == sep) {
            parts.push_back(datePart.substr(start, i - start));
            start = i + 1;
        }
    }
    if (parts.size() != 3) return false;
    for (auto& p : parts) if (trim(p).empty() || !std::all_of(p.begin(), p.end(), [](char c){ return std::isdigit((unsigned char)c); })) return false;
    p1 = std::atoll(parts[0].c_str()); p2 = std::atoll(parts[1].c_str()); p3 = std::atoll(parts[2].c_str());
    if (!parse3(p1, p2, p3)) return false;
    int hh = 0, mm = 0, ss = 0;
    if (!timePart.empty()) {
        std::vector<std::string> tp;
        size_t st = 0;
        for (size_t i = 0; i <= timePart.size(); i++) {
            if (i == timePart.size() || timePart[i] == ':') { tp.push_back(timePart.substr(st, i - st)); st = i + 1; }
        }
        if (tp.size() < 2 || tp.size() > 3) return false;
        for (auto& p : tp) if (p.empty() || !std::all_of(p.begin(), p.end(), [](char c){ return std::isdigit((unsigned char)c); })) return false;
        hh = std::atoi(tp[0].c_str()); mm = std::atoi(tp[1].c_str());
        if (tp.size() == 3) ss = std::atoi(tp[2].c_str());
        if (hasAMPM) {
            if (ampm == "pm" && hh < 12) hh += 12;
            if (ampm == "am" && hh == 12) hh = 0;
        }
        if (hh > 23 || mm > 59 || ss > 59) return false;
    }
    out = makeSerial(y, m, d, hh, mm, ss);
    return true;
}

bool parseDateString(const std::string& s, double& outSerial) {
    return parseOneDate(s, outSerial);
}

std::string formatDate(double serial) {
    long long days = (long long)std::floor(serial) - VB_EPOCH_OFF;
    long long y; unsigned m, d;
    civilFromDays(days, y, m, d);
    return std::format("{}/{}/{}", m, d, y);
}

std::string formatTimeOfDay(double serial) {
    double frac = serial - std::floor(serial);
    long long secs = (long long)std::llround(frac * 86400.0);
    if (secs >= 86400) secs = 86399;
    int hh = (int)(secs / 3600), mm = (int)((secs % 3600) / 60), ss = (int)(secs % 60);
    std::string ampm = hh < 12 ? "AM" : "PM";
    int h12 = hh % 12; if (h12 == 0) h12 = 12;
    return std::format("{}:{:02}:{:02} {}", h12, mm, ss, ampm);
}

std::string formatDateTimeV(double serial) {
    double fl = std::floor(serial);
    double frac = serial - fl;
    // VB's general date format: the date part is left out when it is zero
    // (12/30/1899), so CDate(0) shows "12:00:00 AM"; otherwise the time part is
    // left out when it is zero (midnight).
    if (fl == 0) return formatTimeOfDay(serial);      // pure time: "1:30:15 PM"
    if (frac == 0) return formatDate(serial);        // pure date: "2/29/2024"
    return formatDate(serial) + " " + formatTimeOfDay(serial);
}

// ---------- coercions ----------

bool parseNumericString(const std::string& sin, double& out) {
    std::string s = trim(sin);
    if (s.empty()) { out = 0; return true; }
    // hex / octal literals as strings: &H1A, &O17, &HFFFF&, ...
    // trailing '&' => Long (32-bit wrap); no suffix => Integer (16-bit wrap)
    auto basedStr = [&](int base) -> bool {
        std::string d = s.substr(2);
        bool suffix = !d.empty() && d.back() == '&';
        if (suffix) d.pop_back();
        if (d.empty()) return false;
        char* end = nullptr;
        unsigned long long v = std::strtoull(d.c_str(), &end, base);
        if (!end || *end != 0) return false;
        if (!suffix && v <= 0xFFFFULL) out = (double)(int16_t)(uint16_t)v;
        else if (v <= 0xFFFFFFFFULL) out = (double)(int32_t)(uint32_t)v;
        else out = (double)v;
        return true;
    };
    if (s.size() > 2 && s[0] == '&' && (s[1] == 'h' || s[1] == 'H')) return basedStr(16);
    if (s.size() > 2 && s[0] == '&' && (s[1] == 'o' || s[1] == 'O')) return basedStr(8);
    char* end = nullptr;
    double v = std::strtod(s.c_str(), &end);
    if (end == s.c_str()) return false;
    while (*end == ' ' || *end == '\t') end++;
    if (*end != 0) return false;
    out = v;
    return true;
}

// Objects with a default property coerce through it (CBool, CStr, CInt, ...).
static Variant unwrapDefault(const Variant& v, int depth = 0) {
    if (depth > 8) return v;
    if (v.t == VT::Object && v.obj && v.obj->hasDefault())
        return unwrapDefault(v.obj->getDefault(), depth + 1);
    return v;
}

double toDouble(const Variant& v0) {
    Variant v = unwrapDefault(v0);
    switch (v.t) {
        case VT::Empty: return 0;
        case VT::Null:  throwErr(94, errDesc(94));
        case VT::Bool:  return v.b ? -1.0 : 0.0;
        case VT::Int:
        case VT::Long:
        case VT::Byte:  return (double)v.ll;
        case VT::Currency: return (double)v.ll / (double)CY_SCALE;
        case VT::Double:
        case VT::Single:
        case VT::Date:  return v.d;
        case VT::String: {
            double d;
            if (!parseNumericString(v.s, d)) throwErr(13, errDesc(13));
            return d;
        }
        default: throwErr(13, errDesc(13));
    }
}

long long toInt64(const Variant& v) { return (long long)toDouble(v); }

bool toBool(const Variant& v0) {
    Variant v = unwrapDefault(v0);
    switch (v.t) {
        case VT::Empty: return false;
        case VT::Null:  return false; // VariantChangeType(Null -> Bool) is False
        case VT::Bool:  return v.b;
        case VT::Int:
        case VT::Long:
        case VT::Byte:
        case VT::Currency: return v.ll != 0;
        case VT::Double:
        case VT::Single:
        case VT::Date:  return v.d != 0;
        case VT::String: {
            std::string s = lower(trim(v.s));
            if (s == "true") return true;
            if (s == "false") return false;
            // OLE VarBoolFromStr also accepts the literal strings #TRUE#/#FALSE#
            if (trim(v.s) == "#TRUE#") return true;
            if (trim(v.s) == "#FALSE#") return false;
            double d;
            if (parseNumericString(v.s, d)) return d != 0;
            throwErr(13, errDesc(13));
        }
        case VT::Object:
            if (!v.obj) throwErr(91, errDesc(91)); // Nothing used as a Boolean
            throwErr(13, errDesc(13));
        default: throwErr(13, errDesc(13));
    }
}

// VB prints a Double with up to 15 significant digits and a Single with up to
// 7 (the "%.15G" / "%.7G" shapes of VarBstrFromR8 / VarBstrFromR4).
static std::string formatSig(double d, int digits) {
    if (std::isnan(d)) return "NaN";
    if (std::isinf(d)) return d > 0 ? "Infinity" : "-Infinity";
    if (d == 0) return "0";
    double lim = digits == 7 ? 1e7 : 1e15;
    if (d == std::floor(d) && std::fabs(d) < lim) {
        return std::format("{}", (long long)d);
    }
    std::string s = std::format("{:.{}g}", d, digits);
    // normalize exponent style to VB: 1e+20 -> 1E+20
    auto e = s.find('e');
    if (e != std::string::npos) {
        std::string mant = s.substr(0, e);
        std::string ex = s.substr(e + 1);
        if (!ex.empty() && ex[0] != '-' && ex[0] != '+') ex = "+" + ex;
        if (ex.size() == 2) ex = ex.substr(0, 1) + "0" + ex.substr(1); // +5 -> +05
        s = mant + "E" + ex;
    }
    return s;
}

std::string formatDouble(double d) { return formatSig(d, 15); }
std::string formatSingle(double d) { return formatSig(d, 7); }

std::string formatCurrency(long long scaled) {
    // magnitude as unsigned so the most negative value has no overflow
    unsigned long long m = scaled < 0 ? 0ULL - (unsigned long long)scaled : (unsigned long long)scaled;
    std::string s = scaled < 0 ? "-" : "";
    s += std::format("{}", m / (unsigned long long)CY_SCALE);
    unsigned long long frac = m % (unsigned long long)CY_SCALE;
    if (frac) {
        std::string f = std::format("{:04}", frac);
        while (f.back() == '0') f.pop_back();
        s += "." + f;
    }
    return s;
}

// ---------- Byte / Single / Currency coercions ----------

// banker's rounding of an exact integer quotient p / q (q > 0)
static __int128 roundDivEven(__int128 p, __int128 q) {
    bool neg = p < 0;
    __int128 a = neg ? -p : p;
    __int128 quo = a / q, rem = a % q;
    if (rem * 2 > q || (rem * 2 == q && (quo & 1))) quo++;
    return neg ? -quo : quo;
}

static bool fitsCurrency(__int128 x) { return x >= (__int128)LLONG_MIN && x <= (__int128)LLONG_MAX; }

long long curScaledFromDouble(double d) {
    // VarCyFromR8: scale by 10,000, then banker's rounding to an integer
    if (std::isnan(d)) throwErr(6, errDesc(6));
    double s = d * (double)CY_SCALE;
    if (!(s >= -9223372036854775808.0 && s < 9223372036854775808.0)) throwErr(6, errDesc(6));
    double f = std::floor(s);
    double diff = s - f;
    long long base = (long long)f;
    if (diff > 0.5) return base + 1;
    if (diff < 0.5) return base;
    return (base % 2 == 0) ? base : base + 1;
}

// Plain decimal strings ("123.45678", "-0.5") are converted exactly, so the
// full Currency range survives CCur("922337203685477.5807"); anything else
// (exponents, &H literals) goes through the Double parser.
static bool curScaledFromDecimalString(const std::string& in, long long& out) {
    std::string s = trim(in);
    size_t i = 0;
    bool neg = false;
    if (i < s.size() && (s[i] == '+' || s[i] == '-')) { neg = s[i] == '-'; i++; }
    __int128 ip = 0;
    bool any = false;
    while (i < s.size() && std::isdigit((unsigned char)s[i])) {
        ip = ip * 10 + (s[i] - '0');
        if (ip > (__int128)LLONG_MAX) return false;   // let the Double path raise 6
        any = true; i++;
    }
    __int128 frac = 0;     // first four fraction digits
    int nfrac = 0;
    int roundDigit = -1;   // fifth fraction digit
    bool restNonZero = false;
    if (i < s.size() && s[i] == '.') {
        i++;
        while (i < s.size() && std::isdigit((unsigned char)s[i])) {
            int dg = s[i] - '0';
            if (nfrac < 4) { frac = frac * 10 + dg; nfrac++; }
            else if (roundDigit < 0) roundDigit = dg;
            else if (dg) restNonZero = true;
            any = true; i++;
        }
    }
    if (!any || i != s.size()) return false;
    while (nfrac < 4) { frac *= 10; nfrac++; }
    __int128 v = ip * CY_SCALE + frac;
    if (roundDigit > 5 || (roundDigit == 5 && (restNonZero || (v & 1)))) v++;
    if (neg) v = -v;
    if (!fitsCurrency(v)) throwErr(6, errDesc(6));
    out = (long long)v;
    return true;
}

long long toCurrencyScaled(const Variant& v) {
    switch (v.t) {
        case VT::Empty: return 0;
        case VT::Null:  throwErr(94, errDesc(94));
        case VT::Bool:  return v.b ? -CY_SCALE : 0;
        case VT::Int:
        case VT::Long:
        case VT::Byte:  return v.ll * CY_SCALE; // |ll| < 2^31, cannot overflow
        case VT::Currency: return v.ll;
        case VT::Double:
        case VT::Single:
        case VT::Date:  return curScaledFromDouble(v.d);
        case VT::String: {
            long long r;
            if (curScaledFromDecimalString(v.s, r)) return r;
            return curScaledFromDouble(toDouble(v));
        }
        case VT::Object:
            if (v.obj && v.obj->hasDefault()) return toCurrencyScaled(v.obj->getDefault());
            throwErr(13, errDesc(13));
        default: throwErr(13, errDesc(13));
    }
}

double singleFromDouble(double d) {
    if (std::isnan(d)) return d;
    // values that round to a finite float are kept; the rest overflow
    // (FLT_MAX plus half a unit in the last place rounds to infinity)
    if (std::fabs(d) >= 3.4028235677973366e38) throwErr(6, errDesc(6));
    return (double)(float)d;
}

double toSingleValue(const Variant& v) {
    if (v.t == VT::Single) return v.d;
    return singleFromDouble(toDouble(v));
}

long long toByteValue(const Variant& v) {
    if (v.t == VT::Byte) return v.ll;
    double d = toDouble(v);
    if (!(d >= -0.5 && d <= 255.5)) throwErr(6, errDesc(6));
    long long r = bankerRound(d);
    if (r < 0 || r > 255) throwErr(6, errDesc(6));
    return r;
}

long long toLongValue(const Variant& v) {
    switch (v.t) {
        case VT::Int: case VT::Long: case VT::Byte: return v.ll;
        case VT::Bool: return v.b ? -1 : 0;
        case VT::Empty: return 0;
        case VT::Currency: {
            __int128 r = roundDivEven(v.ll, CY_SCALE);
            if (r < INT32_MIN || r > INT32_MAX) throwErr(6, errDesc(6));
            return (long long)r;
        }
        default: break;
    }
    double d = toDouble(v);
    if (!(d > -2147483649.0 && d < 2147483648.0)) throwErr(6, errDesc(6));
    long long r = bankerRound(d);
    if (r < INT32_MIN || r > INT32_MAX) throwErr(6, errDesc(6));
    return r;
}

std::string toStr(const Variant& v0) {
    Variant v = unwrapDefault(v0);
    switch (v.t) {
        case VT::Empty: return "";
        case VT::Null:  throwErr(94, errDesc(94));
        case VT::Bool:  return v.b ? "True" : "False";
        case VT::Int:
        case VT::Long:
        case VT::Byte:  return std::format("{}", v.ll);
        case VT::Double: return formatDouble(v.d);
        case VT::Single: return formatSingle(v.d);
        case VT::Currency: return formatCurrency(v.ll);
        case VT::Date:  return formatDateTimeV(v.d);
        case VT::String: return v.s;
        case VT::Object:
            if (!v.obj) return "Nothing";
            return v.obj->toString();
        case VT::Array: throwErr(13, errDesc(13));
    }
    return "";
}

std::string toStrLenient(const Variant& v) {
    if (v.t == VT::Null) return "";
    return toStr(v);
}

long long bankerRound(double d) {
    double f = std::floor(d);
    double diff = d - f;
    long long base = (long long)f;
    if (diff > 0.5) return base + 1;
    if (diff < 0.5) return base;
    return (base % 2 == 0) ? base : base + 1;
}

int varTypeOf(const Variant& v) {
    switch (v.t) {
        case VT::Empty:  return 0;
        case VT::Null:   return 1;
        case VT::Int:    return 2;
        case VT::Long:   return 3;
        case VT::Double: return 5;
        case VT::Date:   return 7;
        case VT::String: return 8;
        case VT::Object: return v.obj ? 9 : 9;
        case VT::Bool:   return 11;
        case VT::Single: return 4;
        case VT::Currency: return 6;
        case VT::Byte:   return 17;
        case VT::Array: {
            if (!v.arr || !v.arr->allocated || v.arr->data.empty()) return 8192 + 12;
            return 8192 + varTypeOf(v.arr->data[0]);
        }
    }
    return 12;
}

std::string typeNameOf(const Variant& v) {
    switch (v.t) {
        case VT::Empty:  return "Empty";
        case VT::Null:   return "Null";
        case VT::Int:    return "Integer";
        case VT::Long:   return "Long";
        case VT::Double: return "Double";
        case VT::Date:   return "Date";
        case VT::String: return "String";
        case VT::Bool:   return "Boolean";
        case VT::Byte:   return "Byte";
        case VT::Single: return "Single";
        case VT::Currency: return "Currency";
        case VT::Object: return v.obj ? v.obj->objectKind() : "Nothing";
        case VT::Array: {
            if (v.arr && v.arr->allocated && !v.arr->data.empty())
                return typeNameOf(v.arr->data[0]) + "()";
            return "Variant()";
        }
    }
    return "Variant";
}

// ---------- VArray ----------

void ownArrays(Variant& v) {
    if (v.t != VT::Array || !v.arr) return;
    if (v.arr.use_count() > 1) {
        v.arr = std::make_shared<VArray>(*v.arr);
        v.arr->fixed = false; // a copy of a fixed-size array can be ReDim'd
    }
    for (auto& e : v.arr->data)
        if (e.t == VT::Array) ownArrays(e);
}

long long VArray::totalSize() const {
    // overflow-checked product; dims are validated in alloc()
    long long n = 1;
    for (auto d : dims) {
        if (d < 0) throwErr(5, errDesc(5));
        if (d != 0 && n > LLONG_MAX / d) throwErr(7, errDesc(7));
        n *= d;
    }
    return n;
}

long long VArray::offsetOf(const std::vector<long long>& idx) const {
    if (!allocated) throwErr(9, errDesc(9));
    if (idx.size() != dims.size()) throwErr(9, errDesc(9));
    long long off = 0, stride = 1;
    for (size_t i = dims.size(); i-- > 0;) {
        long long ix = idx[i] - base;
        if (ix < 0 || ix >= dims[i]) throwErr(9, errDesc(9));
        off += ix * stride;
        stride *= dims[i];
    }
    return off;
}

Variant& VArray::at(const std::vector<long long>& idx) {
    return data[(size_t)offsetOf(idx)];
}
const Variant& VArray::at(const std::vector<long long>& idx) const {
    return data[(size_t)offsetOf(idx)];
}

void VArray::alloc(const std::vector<long long>& newDims, int newBase, bool preserve) {
    // validate before mutating: cap total elements so the product cannot
    // overflow, cannot exceed size_t (32-bit targets), and cannot OOM-kill
    // the process before bad_alloc is reachable; huge requests raise "Out of
    // memory" instead of corrupting the heap (previously `Dim a(2^32, 2^32)`
    // wrapped the size product to 0 and element writes went out of bounds)
    constexpr long long MAX_ELEMENTS = 1LL << 24; // 16,777,216 elements
    long long n = 1;
    for (auto d : newDims) {
        if (d < 0) throwErr(5, errDesc(5));
        if (d != 0 && n > MAX_ELEMENTS / d) throwErr(7, errDesc(7));
        n *= d;
    }
    if ((unsigned long long)n > (unsigned long long)std::numeric_limits<size_t>::max())
        throwErr(7, errDesc(7));
    base = newBase;
    std::vector<long long> oldDims = dims;
    std::deque<Variant> oldData = std::move(data);
    dims = newDims;
    data.clear();
    data.resize((size_t)n);
    allocated = true;
    if (preserve && !oldData.empty()) {
        // copy overlapping region (row-major, last dim varies fastest)
        std::vector<long long> idx(dims.size(), 0);
        long long n = totalSize();
        // iterate over new array in order; map to old coords
        for (long long linear = 0; linear < n; linear++) {
            // decode linear -> idx (row-major)
            long long rem = linear;
            for (size_t i = dims.size(); i-- > 0;) {
                idx[i] = rem % dims[i];
                rem /= dims[i];
            }
            bool inOld = idx.size() == oldDims.size();
            if (inOld) {
                for (size_t i = 0; i < idx.size(); i++)
                    if (idx[i] >= oldDims[i]) { inOld = false; break; }
            }
            if (inOld) {
                long long oldOff = 0, stride = 1;
                for (size_t i = oldDims.size(); i-- > 0;) {
                    oldOff += idx[i] * stride;
                    stride *= oldDims[i];
                }
                data[(size_t)linear] = oldData[(size_t)oldOff];
            }
        }
    }
}

// ---------- operators ----------
//
// Result subtypes follow the Variant rules of OLE Automation (VarAdd, VarMul,
// ...) that VBScript uses, as written down in MS-VBAL 5.6.9.3 and the VBA
// operator reference:
//   * the result takes the most precise operand type; for + and - the order
//     is Byte < Integer < Long < Single < Double < Currency (Boolean and
//     Empty count as Integer, a String as Double), and Date wins over all;
//   * * puts Currency below Double: Currency * Single/Double/String is Double;
//   * a Single combined with a Long is a Double;
//   * a Byte, Integer, Long or Single result that leaves its range moves to
//     the narrowest of Integer, Long or Double that holds it, while Currency
//     and Double overflow raise error 6;
//   * / is Double unless a Single meets Byte/Integer/Boolean/Single (Single);
//   * \ and Mod are Byte for two Bytes, Integer while only Byte, Integer and
//     Boolean are involved, Long otherwise;
//   * ^ is always Double.

static bool isStringy(const Variant& v) { return v.t == VT::String; }

static bool isIntFamily(VT t) {
    return t == VT::Byte || t == VT::Bool || t == VT::Int || t == VT::Long || t == VT::Empty;
}

// Operand subtype for arithmetic; objects with a default property take the
// subtype of that value. Arrays and plain objects raise 13 later, through toDouble.
static Variant arithOperand(const Variant& v) {
    if (v.t == VT::Object) {
        if (!v.obj) throwErr(91, errDesc(91)); // Nothing in arithmetic
        if (v.obj->hasDefault()) return arithOperand(v.obj->getDefault());
    }
    return v;
}

enum class ArOp { Add, Sub, Mul, Div, IDiv };

static VT effectiveType(VT a, VT b, ArOp op) {
    auto has = [&](VT t) { return a == t || b == t; };
    if (op == ArOp::Div) {
        if (isIntFamily(a) && isIntFamily(b)) return VT::Double;
        if (has(VT::Double) || has(VT::String) || has(VT::Currency) || has(VT::Date)) return VT::Double;
        return has(VT::Long) ? VT::Double : VT::Single; // a Single is involved
    }
    if (op == ArOp::IDiv) {
        if (a == VT::Byte && b == VT::Byte) return VT::Byte;
        if (has(VT::Long) || has(VT::Single) || has(VT::Double) || has(VT::String) ||
            has(VT::Currency) || has(VT::Date)) return VT::Long;
        return VT::Int;
    }
    if (has(VT::Date)) return op == ArOp::Mul ? VT::Double : VT::Date;
    if (has(VT::Currency)) {
        if (op == ArOp::Mul && (has(VT::Single) || has(VT::Double) || has(VT::String))) return VT::Double;
        return VT::Currency;
    }
    if (has(VT::Double) || has(VT::String)) return VT::Double;
    if (has(VT::Single)) return has(VT::Long) ? VT::Double : VT::Single;
    if (has(VT::Long)) return VT::Long;
    if (has(VT::Int) || has(VT::Bool)) return VT::Int;
    if (has(VT::Byte)) return VT::Byte;
    return VT::Int; // Empty with Empty
}

// value of a Byte/Boolean/Integer/Long/Empty operand
static long long intValue(const Variant& v) {
    if (v.t == VT::Bool) return v.b ? -1 : 0;
    if (v.t == VT::Empty) return 0;
    return v.ll;
}

// an exact integer result of effective type Byte, Integer or Long: kept in that
// type when it fits, otherwise promoted to Integer, Long or Double
static Variant intResult(long long r, VT eff) {
    if (eff == VT::Byte && r >= 0 && r <= 255) return Variant::mkByte(r);
    if ((eff == VT::Byte || eff == VT::Int) && r >= -32768 && r <= 32767) return Variant::mkI2(r);
    if (r >= INT32_MIN && r <= INT32_MAX) return Variant::mkLong(r);
    return Variant::mkDouble((double)r);
}

static Variant doubleResult(double r) {
    if (std::isinf(r)) throwErr(6, errDesc(6));
    return Variant::mkDouble(r);
}

// a Single result that leaves the float range becomes a Double
static Variant singleResult(double r) {
    if (std::isnan(r)) return Variant::mkSingle(r);
    if (std::fabs(r) >= 3.4028235677973366e38) return doubleResult(r);
    return Variant::mkSingle(r);
}

static Variant curResult(__int128 scaled) {
    if (!fitsCurrency(scaled)) throwErr(6, errDesc(6));
    return Variant::mkCur((long long)scaled);
}

static Variant arith(const Variant& a0, const Variant& b0, ArOp op) {
    Variant a = arithOperand(a0), b = arithOperand(b0);
    if (a.isNull() || b.isNull()) return Variant::mkNull();
    if (a.t == VT::Array || b.t == VT::Array || a.t == VT::Object || b.t == VT::Object)
        throwErr(13, errDesc(13));
    VT eff = effectiveType(a.t, b.t, op);
    switch (eff) {
        case VT::Byte:
        case VT::Int:
        case VT::Long: {
            long long x, y;
            if (op == ArOp::IDiv) { x = toLongValue(a); y = toLongValue(b); }
            else { x = intValue(a); y = intValue(b); }
            switch (op) {
                case ArOp::Add: return intResult(x + y, eff);
                case ArOp::Sub: return intResult(x - y, eff);
                case ArOp::Mul: return intResult(x * y, eff);
                default: break; // \ and Mod are done by the callers
            }
            break;
        }
        case VT::Single: {
            double x = toSingleValue(a), y = toSingleValue(b);
            switch (op) {
                case ArOp::Add: return singleResult(x + y);
                case ArOp::Sub: return singleResult(x - y);
                case ArOp::Mul: return singleResult(x * y);
                case ArOp::Div:
                    if (y == 0) throwErr(11, errDesc(11));
                    return singleResult(x / y);
                default: break;
            }
            break;
        }
        case VT::Double: {
            double x = toDouble(a), y = toDouble(b);
            switch (op) {
                case ArOp::Add: return doubleResult(x + y);
                case ArOp::Sub: return doubleResult(x - y);
                case ArOp::Mul: return doubleResult(x * y);
                case ArOp::Div:
                    if (y == 0) throwErr(11, errDesc(11));
                    return doubleResult(x / y);
                default: break;
            }
            break;
        }
        case VT::Currency: {
            __int128 x = toCurrencyScaled(a), y = toCurrencyScaled(b);
            switch (op) {
                case ArOp::Add: return curResult(x + y);
                case ArOp::Sub: return curResult(x - y);
                case ArOp::Mul: return curResult(roundDivEven(x * y, CY_SCALE));
                default: break;
            }
            break;
        }
        case VT::Date: {
            double x = toDouble(a), y = toDouble(b);
            if (op == ArOp::Add) return Variant::mkDate(x + y);
            if (op == ArOp::Sub) {
                if (a.t == VT::Date && b.t == VT::Date) return Variant::mkDouble(x - y); // Date - Date -> Double
                return Variant::mkDate(x - y);
            }
            break;
        }
        default: break;
    }
    throwErr(13, errDesc(13));
}

Variant opAdd(const Variant& a, const Variant& b) {
    if (a.isNull() || b.isNull()) return Variant::mkNull();
    if (isStringy(a) && isStringy(b)) return Variant::mkStr(a.s + b.s);
    return arith(a, b, ArOp::Add);
}

Variant opConcat(const Variant& a, const Variant& b) {
    // Only Null & Null is Null; a single Null operand counts as "" (like Empty).
    // Nothing (a null object) is an error 91 here.
    if (a.isNull() && b.isNull()) return Variant::mkNull();
    if (a.t == VT::Object && !a.obj) throwErr(91, errDesc(91));
    if (b.t == VT::Object && !b.obj) throwErr(91, errDesc(91));
    return Variant::mkStr((a.isNull() ? std::string() : toStr(a)) +
                          (b.isNull() ? std::string() : toStr(b)));
}

Variant opSub(const Variant& a, const Variant& b) { return arith(a, b, ArOp::Sub); }
Variant opMul(const Variant& a, const Variant& b) { return arith(a, b, ArOp::Mul); }
Variant opDiv(const Variant& a, const Variant& b) { return arith(a, b, ArOp::Div); }

// \ and Mod: both operands are rounded (banker's) to the effective integer type
static Variant intDivMod(const Variant& a0, const Variant& b0, bool mod) {
    Variant a = arithOperand(a0), b = arithOperand(b0);
    if (a.isNull() || b.isNull()) return Variant::mkNull();
    if (a.t == VT::Array || b.t == VT::Array || a.t == VT::Object || b.t == VT::Object)
        throwErr(13, errDesc(13));
    VT eff = effectiveType(a.t, b.t, ArOp::IDiv);
    long long x = toLongValue(a), y = toLongValue(b);
    if (y == 0) throwErr(11, errDesc(11));
    // C++ / truncates toward zero and % keeps the dividend's sign, like VB
    return intResult(mod ? x % y : x / y, eff);
}

Variant opIDiv(const Variant& a, const Variant& b) { return intDivMod(a, b, false); }
Variant opMod(const Variant& a, const Variant& b) { return intDivMod(a, b, true); }

Variant opPow(const Variant& a, const Variant& b) {
    if (a.isNull() || b.isNull()) return Variant::mkNull();
    double x = toDouble(arithOperand(a)), y = toDouble(arithOperand(b));
    if (x == 0 && y < 0) throwErr(11, errDesc(11));
    if (x < 0 && y != std::floor(y)) throwErr(5, errDesc(5));
    return doubleResult(std::pow(x, y)); // ^ is always a Double
}

Variant opNeg(const Variant& a0) {
    Variant a = arithOperand(a0);
    if (a.isNull()) return Variant::mkNull();
    switch (a.t) {
        case VT::Byte:                              // -Byte is an Integer
        case VT::Bool:
        case VT::Empty:
        case VT::Int:   return intResult(-intValue(a), VT::Int);
        case VT::Long:  return intResult(-a.ll, VT::Long);
        case VT::Single: return Variant::mkSingle(-a.d);
        case VT::Currency: return curResult(-(__int128)a.ll);
        case VT::Date:  return Variant::mkDate(-a.d);
        case VT::Double: return Variant::mkDouble(-a.d);
        case VT::String: return doubleResult(-toDouble(a));
        default: throwErr(13, errDesc(13));
    }
}

// ---- logical / bitwise operators (And Or Xor Eqv Imp Not) ----
//
// As in VBScript: Boolean with Boolean is a Boolean operation; otherwise the
// operation is bitwise. Empty counts as a Long 0, a numeric string as a Long,
// any other string converts to Boolean (or raises 13), and Single, Double,
// Currency and Date are rounded to a Long. The result is a Byte for two Bytes,
// an Integer while only Byte, Integer and Boolean are involved, else a Long.

static Variant logicOperand(const Variant& v) {
    switch (v.t) {
        case VT::Null: case VT::Bool: case VT::Byte: case VT::Int: case VT::Long:
            return v;
        case VT::Empty: return Variant::mkLong(0);
        case VT::String: {
            double d;
            if (parseNumericString(v.s, d)) return Variant::mkLong(toLongValue(Variant::mkDouble(d)));
            return Variant::mkBool(toBool(v));
        }
        case VT::Object:
            if (!v.obj) throwErr(91, errDesc(91)); // Not Nothing etc.
            if (v.obj->hasDefault()) return logicOperand(v.obj->getDefault());
            throwErr(13, errDesc(13));
        case VT::Array: throwErr(13, errDesc(13));
        default: return Variant::mkLong(toLongValue(v)); // Single, Double, Currency, Date
    }
}

static long long bitsOf(const Variant& v) { return v.t == VT::Bool ? (v.b ? -1 : 0) : v.ll; }

// pack a 32-bit bit pattern into a value of subtype t
static Variant bitsResult(int32_t bits, VT t) {
    if (t == VT::Bool) return Variant::mkBool(bits != 0);
    if (t == VT::Byte) return Variant::mkByte(bits & 0xFF);
    if (t == VT::Int) return Variant::mkI2((int16_t)bits);
    return Variant::mkLong(bits);
}

static VT logicResultType(VT a, VT b) {
    if (a == VT::Bool && b == VT::Bool) return VT::Bool;
    if (a == VT::Byte && b == VT::Byte) return VT::Byte;
    auto small = [](VT t) { return t == VT::Byte || t == VT::Int || t == VT::Bool; };
    if (small(a) && small(b)) return VT::Int;
    return VT::Long;
}

static int32_t allOnes(VT t) { return t == VT::Byte ? 0xFF : -1; }

Variant opNot(const Variant& a0) {
    Variant a = logicOperand(a0);
    if (a.isNull()) return Variant::mkNull();
    return bitsResult(~(int32_t)bitsOf(a), a.t);
}

Variant opLogic(const std::string& op, const Variant& a0, const Variant& b0) {
    Variant a = logicOperand(a0), b = logicOperand(b0);
    if (a.isNull() || b.isNull()) {
        if (a.isNull() && b.isNull()) return Variant::mkNull();
        // three-valued logic, bit by bit, against an unknown (Null) operand
        bool leftKnown = !a.isNull();
        const Variant& k = leftKnown ? a : b;
        int32_t x = (int32_t)bitsOf(k);
        int32_t ones = allOnes(k.t);
        if (op == "and" && x == 0) return bitsResult(0, k.t);
        if (op == "or" && (x & ones) == ones) return bitsResult(ones, k.t);
        if (op == "imp") {
            if (leftKnown) {
                if (x == 0) return bitsResult(ones, k.t);           // False Imp _ = True
                if (k.t == VT::Byte) return bitsResult(~x, VT::Byte); // VBScript keeps Byte: Not x
            } else if ((x & ones) == ones) {
                return bitsResult(ones, k.t);                       // _ Imp True = True
            }
        }
        return Variant::mkNull();
    }
    VT rt = logicResultType(a.t, b.t);
    int32_t x = (int32_t)bitsOf(a), y = (int32_t)bitsOf(b);
    int32_t r;
    if (op == "and") r = x & y;
    else if (op == "or") r = x | y;
    else if (op == "xor") r = x ^ y;
    else if (op == "eqv") r = ~(x ^ y);
    else if (op == "imp") r = ~x | y;
    else throwErr(13, errDesc(13));
    return bitsResult(r, rt);
}

// returns -1/0/1 for ordering; throws on type problems
static int cmpNumStr(const Variant& a, const Variant& b, bool compareText) {
    bool aStr = a.t == VT::String, bStr = b.t == VT::String;
    if (aStr && bStr) {
        if (compareText) {
            std::string x = lower(a.s), y = lower(b.s);
            return x < y ? -1 : (x > y ? 1 : 0);
        }
        return a.s < b.s ? -1 : (a.s > b.s ? 1 : 0);
    }
    if (aStr != bStr) {
        // mixed string/non-string: numeric if the string parses as a number,
        // otherwise compare as text (VBScript never errors here, e.g. in
        // Select Case: `case 3` vs `"test"`)
        const Variant& sv = aStr ? a : b;
        const Variant& nv = aStr ? b : a;
        double d;
        if (!trim(sv.s).empty() && parseNumericString(sv.s, d)) {
            double x = toDouble(nv);
            return d < x ? -1 : (d > x ? 1 : 0);
        }
        std::string x = sv.s, y = toStr(nv);
        if (compareText) { x = lower(x); y = lower(y); }
        return x < y ? -1 : (x > y ? 1 : 0);
    }
    // Currency against Currency or a whole-number type compares exactly
    auto cyExact = [](VT t) { return t == VT::Currency || t == VT::Int || t == VT::Long ||
                                     t == VT::Byte || t == VT::Bool || t == VT::Empty; };
    if ((a.t == VT::Currency || b.t == VT::Currency) && cyExact(a.t) && cyExact(b.t)) {
        long long x = toCurrencyScaled(a), y = toCurrencyScaled(b);
        return x < y ? -1 : (x > y ? 1 : 0);
    }
    double x = toDouble(a), y = toDouble(b);
    // a Single compares at Single precision, even against a Double
    // (CSng(0.1) = 0.1 is True), unless the other value is beyond the float range
    if ((a.t == VT::Single || b.t == VT::Single) && !aStr && !bStr &&
        std::fabs(x) < 3.4028235677973366e38 && std::fabs(y) < 3.4028235677973366e38) {
        float fx = (float)x, fy = (float)y;
        return fx < fy ? -1 : (fx > fy ? 1 : 0);
    }
    return x < y ? -1 : (x > y ? 1 : 0);
}

Variant opCompare(const std::string& op, const Variant& a0, const Variant& b0, bool compareText) {
    Variant a = unwrapDefault(a0), b = unwrapDefault(b0);
    if (a.isNull() || b.isNull()) return Variant::mkNull();
    if (a.t == VT::Object || b.t == VT::Object) {
        if (a.t == VT::Object && b.t == VT::Object) {
            bool eq = a.obj == b.obj;
            if (op == "=") return Variant::mkBool(eq);
            if (op == "<>") return Variant::mkBool(!eq);
            throwErr(13, errDesc(13));
        }
        throwErr(424, errDesc(424));
    }
    if (a.t == VT::Array || b.t == VT::Array) throwErr(13, errDesc(13));
    int c = cmpNumStr(a, b, compareText);
    bool r = false;
    if (op == "=") r = c == 0;
    else if (op == "<>") r = c != 0;
    else if (op == "<") r = c < 0;
    else if (op == ">") r = c > 0;
    else if (op == "<=") r = c <= 0;
    else if (op == ">=") r = c >= 0;
    else throwErr(5, errDesc(5));
    return Variant::mkBool(r);
}

bool objSame(const Variant& a, const Variant& b) {
    if (a.t == VT::Object && b.t == VT::Object) return a.obj == b.obj;
    // "x Is Nothing" on an Empty/Null variant is legal and simply False/Null
    if (a.t == VT::Object && b.isEmpty()) return false;
    if (b.t == VT::Object && a.isEmpty()) return false;
    if (a.t == VT::Object && b.isNull()) return false;
    if (b.t == VT::Object && a.isNull()) return false;
    throwErr(424, errDesc(424));
}
