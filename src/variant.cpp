#include "variant.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>

// ---------- errors ----------

std::string errDesc(int num) {
    switch (num) {
        case 5:   return "Invalid procedure call or argument";
        case 6:   return "Overflow";
        case 9:   return "Subscript out of range";
        case 11:  return "Division by zero";
        case 13:  return "Type mismatch";
        case 14:  return "Out of string space";
        case 35:  return "Sub or Function not defined";
        case 92:  return "For loop not initialized";
        case 94:  return "Invalid use of Null";
        case 424: return "Object required";
        case 425: return "Invalid object";
        case 429: return "ActiveX component can't create object";
        case 430: return "Class doesn't support Automation";
        case 438: return "Object doesn't support this property or method";
        case 450: return "Wrong number of arguments or invalid property assignment";
        case 451: return "Object not a collection";
        case 500: return "Variable is undefined";
        case 501: return "Invalid assignment";
        case 5016: return "Syntax error in regular expression";
        case 1001: return "Out of memory";
        default:  return "Runtime error " + std::to_string(num);
    }
}

[[noreturn]] void throwErr(int num, const std::string& desc) {
    throw VBSError(num, desc.empty() ? errDesc(num) : desc);
}

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
    props[n] = v;
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
    if (frac == 0) return formatDate(serial);        // pure date: "2/29/2024"
    if (fl == 0) return formatTimeOfDay(serial);      // pure time: "1:30:15 PM"
    return formatDate(serial) + " " + formatTimeOfDay(serial);
}

// ---------- coercions ----------

bool parseNumericString(const std::string& sin, double& out) {
    std::string s = trim(sin);
    if (s.empty()) { out = 0; return true; }
    // hex / octal literals as strings: &H1A, &O17
    if (s.size() > 2 && s[0] == '&' && (s[1] == 'h' || s[1] == 'H')) {
        char* end = nullptr;
        long long v = std::strtoll(s.c_str() + 2, &end, 16);
        if (end && *end == 0 && end != s.c_str() + 2) { out = (double)v; return true; }
        return false;
    }
    if (s.size() > 2 && s[0] == '&' && (s[1] == 'o' || s[1] == 'O')) {
        char* end = nullptr;
        long long v = std::strtoll(s.c_str() + 2, &end, 8);
        if (end && *end == 0 && end != s.c_str() + 2) { out = (double)v; return true; }
        return false;
    }
    char* end = nullptr;
    double v = std::strtod(s.c_str(), &end);
    if (end == s.c_str()) return false;
    while (*end == ' ' || *end == '\t') end++;
    if (*end != 0) return false;
    out = v;
    return true;
}

double toDouble(const Variant& v) {
    switch (v.t) {
        case VT::Empty: return 0;
        case VT::Null:  throwErr(94, errDesc(94));
        case VT::Bool:  return v.b ? -1.0 : 0.0;
        case VT::Int:
        case VT::Long:  return (double)v.ll;
        case VT::Double:
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

bool toBool(const Variant& v) {
    switch (v.t) {
        case VT::Empty: return false;
        case VT::Null:  throwErr(94, errDesc(94));
        case VT::Bool:  return v.b;
        case VT::Int:
        case VT::Long:  return v.ll != 0;
        case VT::Double:
        case VT::Date:  return v.d != 0;
        case VT::String: {
            std::string s = lower(trim(v.s));
            if (s == "true") return true;
            if (s == "false") return false;
            double d;
            if (parseNumericString(v.s, d)) return d != 0;
            throwErr(13, errDesc(13));
        }
        default: throwErr(13, errDesc(13));
    }
}

std::string formatDouble(double d) {
    if (std::isnan(d)) return "NaN";
    if (std::isinf(d)) return d > 0 ? "Infinity" : "-Infinity";
    if (d == 0) return "0";
    if (d == std::floor(d) && std::fabs(d) < 1e15) {
        return std::format("{}", (long long)d);
    }
    std::string s = std::format("{:.15g}", d);
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

std::string toStr(const Variant& v) {
    switch (v.t) {
        case VT::Empty: return "";
        case VT::Null:  throwErr(94, errDesc(94));
        case VT::Bool:  return v.b ? "True" : "False";
        case VT::Int:
        case VT::Long:  return std::format("{}", v.ll);
        case VT::Double: return formatDouble(v.d);
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

long long VArray::totalSize() const {
    long long n = 1;
    for (auto d : dims) n *= d;
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
    base = newBase;
    std::vector<long long> oldDims = dims;
    std::deque<Variant> oldData = std::move(data);
    dims = newDims;
    data.clear();
    data.resize((size_t)totalSize());
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

static bool isStringy(const Variant& v) { return v.t == VT::String; }

static Variant numResult(double d) {
    if (d == std::floor(d) && std::fabs(d) < 9.0e18) {
        long long ll = (long long)d;
        if (ll >= -2147483648LL && ll <= 2147483647LL) return Variant::mkInt(ll);
        return Variant::mkDouble(d);
    }
    return Variant::mkDouble(d);
}

static Variant checkedIntAdd(long long a, long long b) {
    long long r = a + b;
    if ((b > 0 && r < a) || (b < 0 && r > a)) throwErr(6, errDesc(6));
    return Variant::mkInt(r);
}

Variant opAdd(const Variant& a, const Variant& b) {
    if (a.isNull() || b.isNull()) return Variant::mkNull();
    if (isStringy(a) && isStringy(b)) return Variant::mkStr(a.s + b.s);
    if (a.isIntegral() && b.isIntegral()) return checkedIntAdd(a.ll, b.ll);
    double r = toDouble(a) + toDouble(b);
    if (a.t == VT::Date || b.t == VT::Date) return Variant::mkDate(r);
    return numResult(r);
}

Variant opConcat(const Variant& a, const Variant& b) {
    if (a.isNull() || b.isNull()) return Variant::mkNull();
    return Variant::mkStr(toStr(a) + toStr(b));
}

Variant opSub(const Variant& a, const Variant& b) {
    if (a.isNull() || b.isNull()) return Variant::mkNull();
    if (a.isIntegral() && b.isIntegral()) {
        long long r = a.ll - b.ll;
        if ((b.ll < 0 && r < a.ll) || (b.ll > 0 && r > a.ll)) throwErr(6, errDesc(6));
        return Variant::mkInt(r);
    }
    double r = toDouble(a) - toDouble(b);
    if (a.t == VT::Date && b.t != VT::Date) return Variant::mkDate(r); // Date - number -> Date
    if (a.t == VT::Date && b.t == VT::Date) return Variant::mkDouble(r); // Date - Date -> Double
    return numResult(r);
}

Variant opMul(const Variant& a, const Variant& b) {
    if (a.isNull() || b.isNull()) return Variant::mkNull();
    if (a.isIntegral() && b.isIntegral()) {
        long long r = a.ll * b.ll;
        if (b.ll != 0 && r / b.ll != a.ll) throwErr(6, errDesc(6));
        return Variant::mkInt(r);
    }
    return numResult(toDouble(a) * toDouble(b));
}

Variant opDiv(const Variant& a, const Variant& b) {
    if (a.isNull() || b.isNull()) return Variant::mkNull();
    double db = toDouble(b);
    if (db == 0) throwErr(11, errDesc(11));
    return numResult(toDouble(a) / db);
}

Variant opIDiv(const Variant& a, const Variant& b) {
    if (a.isNull() || b.isNull()) return Variant::mkNull();
    long long ia = bankerRound(toDouble(a));
    long long ib = bankerRound(toDouble(b));
    if (ib == 0) throwErr(11, errDesc(11));
    return Variant::mkLong(ia / ib);
}

Variant opMod(const Variant& a, const Variant& b) {
    if (a.isNull() || b.isNull()) return Variant::mkNull();
    long long ia = bankerRound(toDouble(a));
    long long ib = bankerRound(toDouble(b));
    if (ib == 0) throwErr(11, errDesc(11));
    return Variant::mkLong(ia % ib); // C++ % keeps dividend's sign, like VB Mod
}

Variant opPow(const Variant& a, const Variant& b) {
    if (a.isNull() || b.isNull()) return Variant::mkNull();
    double x = toDouble(a), y = toDouble(b);
    if (x == 0 && y < 0) throwErr(11, errDesc(11));
    double r = std::pow(x, y);
    if (x < 0 && y != std::floor(y)) throwErr(5, errDesc(5));
    return numResult(r);
}

Variant opNeg(const Variant& a) {
    if (a.isNull()) return Variant::mkNull();
    if (a.isIntegral()) return Variant::mkLong(-a.ll);
    return numResult(-toDouble(a));
}

static int32_t bitOf(const Variant& v) {
    if (v.t == VT::Bool) return v.b ? -1 : 0;
    return (int32_t)toInt64(v);
}

Variant opNot(const Variant& a) {
    if (a.isNull()) return Variant::mkNull();
    if (a.t == VT::Bool) return Variant::mkBool(!a.b);
    if (a.isIntegral()) return Variant::mkLong(~(int32_t)a.ll);
    return Variant::mkBool(!toBool(a));
}

Variant opLogic(const std::string& op, const Variant& a, const Variant& b) {
    if (a.isNull() || b.isNull()) return Variant::mkNull();
    bool bothBool = a.t == VT::Bool && b.t == VT::Bool;
    bool bothInt = (a.isIntegral() || a.t == VT::Bool) && (b.isIntegral() || b.t == VT::Bool);
    if (bothBool || !bothInt) {
        // logical
        bool x = toBool(a), y = toBool(b);
        if (op == "and") return Variant::mkBool(x && y);
        if (op == "or")  return Variant::mkBool(x || y);
        if (op == "xor") return Variant::mkBool(x != y);
        if (op == "eqv") return Variant::mkBool(x == y);
        if (op == "imp") return Variant::mkBool(!x || y);
    } else {
        int32_t x = bitOf(a), y = bitOf(b);
        if (op == "and") return Variant::mkLong(x & y);
        if (op == "or")  return Variant::mkLong(x | y);
        if (op == "xor") return Variant::mkLong(x ^ y);
        if (op == "eqv") return Variant::mkLong(~(x ^ y));
        if (op == "imp") return Variant::mkLong(~x | y);
    }
    throwErr(13, errDesc(13));
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
    double x = toDouble(a), y = toDouble(b);
    return x < y ? -1 : (x > y ? 1 : 0);
}

Variant opCompare(const std::string& op, const Variant& a, const Variant& b, bool compareText) {
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
