// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 Lucas Holt. All rights reserved.

#include "builtins.h"
#include "interpreter.h"
#include "objects.h"

#include <cctype>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <iostream>
#include <string>

// ---------- arg helpers ----------

static const Variant& A(std::vector<Variant>& a, size_t i) {
    static const Variant empty;
    return i < a.size() ? a[i] : empty;
}

// ---------- UTF-8 helpers (strings are treated as code-point sequences) ----------

static size_t cpLen(const std::string& s) {
    size_t n = 0;
    for (unsigned char c : s) if ((c & 0xC0) != 0x80) n++;
    return n;
}
static unsigned cpAt(const std::string& s, size_t bytePos, size_t* adv = nullptr) {
    unsigned char c = s[bytePos];
    if (c < 0x80) { if (adv) *adv = 1; return c; }
    unsigned cp = 0; int len = 0;
    if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; len = 2; }
    else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; len = 3; }
    else { cp = c & 0x07; len = 4; }
    for (int i = 1; i < len && bytePos + i < s.size(); i++) cp = (cp << 6) | (s[bytePos + i] & 0x3F);
    if (adv) *adv = (size_t)len;
    return cp;
}
static std::string cpToUtf8(unsigned cp) {
    std::string out;
    if (cp < 0x80) out += (char)cp;
    else if (cp < 0x800) { out += (char)(0xC0 | (cp >> 6)); out += (char)(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) { out += (char)(0xE0 | (cp >> 12)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
    else { out += (char)(0xF0 | (cp >> 18)); out += (char)(0x80 | ((cp >> 12) & 0x3F)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
    return out;
}
// substring by code points, 0-based
static std::string cpSubstr(const std::string& s, size_t start, size_t len) {
    std::string out;
    size_t cp = 0, i = 0;
    while (i < s.size()) {
        size_t adv = 0;
        unsigned c = cpAt(s, i, &adv);
        if (cp >= start && cp < start + len) out += cpToUtf8(c);
        i += adv;
        cp++;
    }
    return out;
}
static std::string asciiCase(const std::string& s, bool up); // fwd
// split string into code-point units (optionally case-folded) for positional string ops
static std::vector<std::string> cpv(const std::string& s, bool fold) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < s.size()) {
        size_t adv = 0;
        unsigned c = cpAt(s, i, &adv);
        std::string u = cpToUtf8(c);
        out.push_back(fold ? asciiCase(u, false) : u);
        i += adv;
    }
    return out;
}
static std::string asciiCase(const std::string& s, bool up) {
    std::string o = s;
    for (auto& c : o) {
        if (c >= 'a' && c <= 'z' && up) c = (char)(c - 'a' + 'A');
        else if (c >= 'A' && c <= 'Z' && !up) c = (char)(c - 'A' + 'a');
    }
    return o;
}
static bool ws(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v'; }
static std::string trimVb(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && ws(s[a])) a++;
    while (b > a && ws(s[b - 1])) b--;
    return s.substr(a, b - a);
}
static std::string trimL(const std::string& s) {
    size_t a = 0; while (a < s.size() && ws(s[a])) a++;
    return s.substr(a);
}
static std::string trimR(const std::string& s) {
    size_t b = s.size(); while (b > 0 && ws(s[b - 1])) b--;
    return s.substr(0, b);
}

static std::shared_ptr<VArray> makeStrArray(std::vector<Variant>&& items) {
    auto arr = std::make_shared<VArray>();
    arr->base = 0;
    arr->dims = {(long long)items.size()};
    arr->data.assign(items.begin(), items.end());
    arr->allocated = true;
    return arr;
}

// ---------- numeric helpers ----------

// Int (mode 0) and Fix (mode 1): the result keeps the argument's subtype
static Variant roundKeepingType(const Variant& v, double (*f)(double), int mode) {
    switch (v.t) {
        case VT::Null:  return Variant::mkNull();
        case VT::Byte: case VT::Int: case VT::Long: return v;
        case VT::Empty: return Variant::mkI2(0);
        case VT::Bool:  return Variant::mkI2(v.b ? -1 : 0);
        case VT::Single: return Variant::mkSingle(f(v.d));
        case VT::Currency: {
            long long q = v.ll / CY_SCALE;                    // truncates toward zero
            if (mode == 0 && v.ll < 0 && v.ll % CY_SCALE != 0) q--; // Int rounds down
            return Variant::mkCur(q * CY_SCALE);
        }
        default: return Variant::mkDouble(f(toDouble(v)));
    }
}

// ---------- date helpers ----------

static void splitSerial(double s, long long& y, unsigned& m, unsigned& d, int& hh, int& mm, int& ss) {
    long long days = (long long)std::floor(s);
    double frac = s - (double)days;
    long long secs = (long long)std::llround(frac * 86400.0);
    if (secs >= 86400) { secs -= 86400; days++; }
    if (secs < 0) { secs += 86400; days--; }
    civilFromDays(days - VB_EPOCH_OFF, y, m, d);
    hh = (int)(secs / 3600); mm = (int)((secs % 3600) / 60); ss = (int)(secs % 60);
}

static double nowSerial() {
    std::time_t t = std::time(nullptr);
    std::tm tmv;
    localtime_r(&t, &tmv);
    return makeSerial(tmv.tm_year + 1900, (unsigned)tmv.tm_mon + 1, (unsigned)tmv.tm_mday,
                      tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
}

static double dateArg(std::vector<Variant>& a, size_t i) {
    if (i >= a.size()) return nowSerial();
    const Variant& v = a[i];
    if (v.t == VT::Date) return v.d;
    if (v.t == VT::String) {
        double s;
        if (parseDateString(v.s, s)) return s;
        throwErr(13, errDesc(13));
    }
    return toDouble(v);
}

static long long normInterval(double n) { return (long long)n; }

static Variant dateAddImpl(const std::string& unit, long long n, double date) {
    long long y; unsigned m, d; int hh, mm, ss;
    splitSerial(date, y, m, d, hh, mm, ss);
    if (unit == "yyyy") {
        y += n;
        if (d > daysInMonth(y, m)) d = daysInMonth(y, m);
    } else if (unit == "q") {
        long long total = y * 12 + (m - 1) + 3 * n;
        y = total / 12; m = (unsigned)(total % 12) + 1;
        if (d > daysInMonth(y, m)) d = daysInMonth(y, m);
    } else if (unit == "m") {
        long long total = y * 12 + (m - 1) + n;
        y = total / 12; m = (unsigned)(total % 12) + 1;
        if (m == 0) { m = 12; y--; }
        if (total < 0 && total % 12 != 0) { /* handled by floor semantics below */ }
        // normalize negative totals
        if (total < 0) { y = (total - 11) / 12; m = (unsigned)(total - y * 12) + 1; }
        if (d > daysInMonth(y, m)) d = daysInMonth(y, m);
    } else if (unit == "y" || unit == "d" || unit == "w") {
        return Variant::mkDate(date + (double)n);
    } else if (unit == "ww") {
        return Variant::mkDate(date + (double)(7 * n));
    } else if (unit == "h") {
        return Variant::mkDate(date + n / 24.0);
    } else if (unit == "n") {
        return Variant::mkDate(date + n / 1440.0);
    } else if (unit == "s") {
        return Variant::mkDate(date + n / 86400.0);
    } else throwErr(5, errDesc(5));
    return Variant::mkDate(makeSerial(y, m, d, hh, mm, ss));
}

static Variant dateDiffImpl(const std::string& unit, double d1, double d2) {
    long long y1, y2; unsigned m1, m2, d1_, d2_; int h1, mi1, s1, h2, mi2, s2;
    splitSerial(d1, y1, m1, d1_, h1, mi1, s1);
    splitSerial(d2, y2, m2, d2_, h2, mi2, s2);
    if (unit == "yyyy") return Variant::mkLong(y2 - y1);
    if (unit == "q") return Variant::mkLong((y2 * 4 + (m2 - 1) / 3) - (y1 * 4 + (m1 - 1) / 3));
    if (unit == "m") return Variant::mkLong((y2 * 12 + m2) - (y1 * 12 + m1));
    if (unit == "y" || unit == "d" || unit == "w") {
        long long days = (long long)std::floor(d2) - (long long)std::floor(d1);
        return Variant::mkLong(days);
    }
    if (unit == "ww") {
        long long days = (long long)std::floor(d2) - (long long)std::floor(d1);
        return Variant::mkLong(days / 7);
    }
    double diff = d2 - d1;
    if (unit == "h") return Variant::mkLong((long long)std::floor(diff * 24));
    if (unit == "n") return Variant::mkLong((long long)std::floor(diff * 1440));
    if (unit == "s") return Variant::mkLong((long long)std::floor(diff * 86400));
    throwErr(5, errDesc(5));
}

static Variant datePartImpl(const std::string& unit, double date) {
    long long y; unsigned m, d; int hh, mm, ss;
    splitSerial(date, y, m, d, hh, mm, ss);
    if (unit == "yyyy") return Variant::mkLong(y);
    if (unit == "q") return Variant::mkLong((m - 1) / 3 + 1);
    if (unit == "m") return Variant::mkLong(m);
    if (unit == "y") return Variant::mkLong((long long)std::floor(date) - (daysFromCivil(y, 1, 1) + VB_EPOCH_OFF) + 1);
    if (unit == "d") return Variant::mkLong(d);
    if (unit == "w") return Variant::mkLong(weekdayOf(date));
    if (unit == "ww") {
        long long doy = (long long)std::floor(date) - (daysFromCivil(y, 1, 1) + VB_EPOCH_OFF) + 1;
        int jan1 = weekdayOf(makeSerial(y, 1, 1, 0, 0, 0));
        return Variant::mkLong((doy + jan1 - 2) / 7 + 1);
    }
    if (unit == "h") return Variant::mkLong(hh);
    if (unit == "n") return Variant::mkLong(mm);
    if (unit == "s") return Variant::mkLong(ss);
    throwErr(5, errDesc(5));
}

// ---------- misc helpers ----------

static std::string fmtNum(double v, const std::string& pattern) {
    // minimal numeric format support: 0 # . , % patterns
    if (pattern.empty()) return formatDouble(v);
    bool percent = pattern.find('%') != std::string::npos;
    if (percent) v *= 100;
    int decimals = 0;
    auto dot = pattern.find('.');
    if (dot != std::string::npos) {
        for (size_t i = dot + 1; i < pattern.size(); i++) {
            char c = pattern[i];
            if (c == '0' || c == '#') decimals++;
            else break;
        }
    }
    bool comma = pattern.find(',') != std::string::npos;
    char buf[128];
    std::snprintf(buf, sizeof buf, "%.*f", decimals, v);
    std::string s = buf;
    if (comma) {
        // insert thousands separators into integer part
        auto p = s.find('.');
        std::string ip = p == std::string::npos ? s : s.substr(0, p);
        std::string fp = p == std::string::npos ? "" : s.substr(p);
        bool neg = !ip.empty() && ip[0] == '-';
        if (neg) ip = ip.substr(1);
        std::string out;
        int cnt = 0;
        for (auto it = ip.rbegin(); it != ip.rend(); ++it) {
            out = *it + out;
            if (++cnt % 3 == 0 && it + 1 != ip.rend()) out = "," + out;
        }
        s = (neg ? "-" : "") + out + fp;
    }
    if (percent) s += "%";
    return s;
}

// ---------- registration ----------

void registerConstants(Interp& ip) {
    auto& c = ip.constants;
    auto add = [&](const std::string& n, Variant v) { c[lower(n)] = std::move(v); };
    add("vbCrLf", Variant::mkStr("\r\n"));
    add("vbCr", Variant::mkStr("\r"));
    add("vbLf", Variant::mkStr("\n"));
    add("vbTab", Variant::mkStr("\t"));
    add("vbVerticalTab", Variant::mkStr("\v"));
    add("vbFormFeed", Variant::mkStr("\f"));
    add("vbBack", Variant::mkStr("\b"));
    add("vbNullString", Variant::mkStr(""));
    add("vbNullChar", Variant::mkStr(std::string("\0", 1)));
    add("vbObjectError", Variant::mkLong(-2147221504LL)); // &H80040000 as Long
    add("vbBinaryCompare", Variant::mkInt(0));
    add("vbTextCompare", Variant::mkInt(1));
    add("vbSunday", Variant::mkInt(1));
    add("vbMonday", Variant::mkInt(2));
    add("vbTuesday", Variant::mkInt(3));
    add("vbWednesday", Variant::mkInt(4));
    add("vbThursday", Variant::mkInt(5));
    add("vbFriday", Variant::mkInt(6));
    add("vbSaturday", Variant::mkInt(7));
    add("vbUseSystem", Variant::mkInt(0));
    add("vbFirstJan1", Variant::mkInt(1));
    add("vbFirstFourDays", Variant::mkInt(2));
    add("vbFirstFullWeek", Variant::mkInt(3));
    // FormatDateTime formats
    add("vbGeneralDate", Variant::mkInt(0));
    add("vbLongDate", Variant::mkInt(1));
    add("vbShortDate", Variant::mkInt(2));
    add("vbLongTime", Variant::mkInt(3));
    add("vbShortTime", Variant::mkInt(4));
    // MsgBox results
    add("vbOK", Variant::mkInt(1));
    add("vbCancel", Variant::mkInt(2));
    add("vbAbort", Variant::mkInt(3));
    add("vbRetry", Variant::mkInt(4));
    add("vbIgnore", Variant::mkInt(5));
    add("vbYes", Variant::mkInt(6));
    add("vbNo", Variant::mkInt(7));
    // MsgBox button sets
    add("vbOKOnly", Variant::mkInt(0));
    add("vbOKCancel", Variant::mkInt(1));
    add("vbAbortRetryIgnore", Variant::mkInt(2));
    add("vbYesNoCancel", Variant::mkInt(3));
    add("vbYesNo", Variant::mkInt(4));
    add("vbRetryCancel", Variant::mkInt(5));
    add("vbCritical", Variant::mkInt(16));
    add("vbQuestion", Variant::mkInt(32));
    add("vbExclamation", Variant::mkInt(48));
    add("vbInformation", Variant::mkInt(64));
    add("vbDefaultButton1", Variant::mkInt(0));
    add("vbDefaultButton2", Variant::mkInt(256));
    add("vbDefaultButton3", Variant::mkInt(512));
    add("vbDefaultButton4", Variant::mkInt(768));
    add("vbApplicationModal", Variant::mkInt(0));
    add("vbSystemModal", Variant::mkInt(4096));
    add("vbMsgBoxHelpButton", Variant::mkInt(16384));
    add("vbMsgBoxSetForeground", Variant::mkInt(65536));
    add("vbMsgBoxRight", Variant::mkInt(524288));
    add("vbMsgBoxRtlReading", Variant::mkInt(1048576));
    // VarType constants
    add("vbEmpty", Variant::mkInt(0));
    add("vbNull", Variant::mkInt(1));
    add("vbInteger", Variant::mkInt(2));
    add("vbLong", Variant::mkInt(3));
    add("vbSingle", Variant::mkInt(4));
    add("vbDouble", Variant::mkInt(5));
    add("vbCurrency", Variant::mkInt(6));
    add("vbDate", Variant::mkInt(7));
    add("vbString", Variant::mkInt(8));
    add("vbObject", Variant::mkInt(9));
    add("vbError", Variant::mkInt(10));
    add("vbBoolean", Variant::mkInt(11));
    add("vbVariant", Variant::mkInt(12));
    add("vbDataObject", Variant::mkInt(13));
    add("vbDecimal", Variant::mkInt(14));
    add("vbByte", Variant::mkInt(17));
    add("vbArray", Variant::mkInt(8192));
    // colors
    add("vbBlack", Variant::mkLong(0x000000));
    add("vbRed", Variant::mkLong(0x0000FF));
    add("vbGreen", Variant::mkLong(0x00FF00));
    add("vbYellow", Variant::mkLong(0x00FFFF));
    add("vbBlue", Variant::mkLong(0xFF0000));
    add("vbMagenta", Variant::mkLong(0xFF00FF));
    add("vbCyan", Variant::mkLong(0xFFFF00));
    add("vbWhite", Variant::mkLong(0xFFFFFF));
    // FileSystemObject constants
    add("vbNormal", Variant::mkInt(0));
    add("vbReadOnly", Variant::mkInt(1));
    add("vbHidden", Variant::mkInt(2));
    add("vbSystem", Variant::mkInt(4));
    add("vbVolume", Variant::mkInt(8));
    add("vbDirectory", Variant::mkInt(16));
    add("vbArchive", Variant::mkInt(32));
    add("vbAlias", Variant::mkInt(64));
    add("ForReading", Variant::mkInt(1));
    add("ForWriting", Variant::mkInt(2));
    add("ForAppending", Variant::mkInt(8));
    add("TristateUseDefault", Variant::mkInt(-2));
    add("TristateTrue", Variant::mkInt(-1));
    add("TristateFalse", Variant::mkInt(0));
    // ADODB.Stream constants
    add("adTypeBinary", Variant::mkInt(1));
    add("adTypeText", Variant::mkInt(2));
    add("adModeUnknown", Variant::mkInt(0));
    add("adModeRead", Variant::mkInt(1));
    add("adModeWrite", Variant::mkInt(2));
    add("adModeReadWrite", Variant::mkInt(3));
    add("adModeShareDenyRead", Variant::mkInt(4));
    add("adModeShareDenyWrite", Variant::mkInt(8));
    add("adModeShareExclusive", Variant::mkInt(12));
    add("adSaveCreateNotExist", Variant::mkInt(1));
    add("adSaveCreateOverWrite", Variant::mkInt(2));
    add("adReadAll", Variant::mkInt(-1));
    add("adReadLine", Variant::mkInt(-2));
    add("adCRLF", Variant::mkInt(-1));
    add("adLF", Variant::mkInt(10));
    add("adCR", Variant::mkInt(13));
    add("adStateClosed", Variant::mkInt(0));
    add("adStateOpen", Variant::mkInt(1));
    // ADODB.DataTypeEnum
    add("adEmpty", Variant::mkInt(0));
    add("adTinyInt", Variant::mkInt(16));
    add("adSmallInt", Variant::mkInt(2));
    add("adInteger", Variant::mkInt(3));
    add("adBigInt", Variant::mkInt(20));
    add("adUnsignedTinyInt", Variant::mkInt(17));
    add("adUnsignedSmallInt", Variant::mkInt(18));
    add("adUnsignedInt", Variant::mkInt(19));
    add("adUnsignedBigInt", Variant::mkInt(21));
    add("adSingle", Variant::mkInt(4));
    add("adDouble", Variant::mkInt(5));
    add("adCurrency", Variant::mkInt(6));
    add("adDecimal", Variant::mkInt(131));
    add("adNumeric", Variant::mkInt(131));
    add("adBoolean", Variant::mkInt(11));
    add("adError", Variant::mkInt(10));
    add("adGUID", Variant::mkInt(72));
    add("adDate", Variant::mkInt(7));
    add("adDBDate", Variant::mkInt(133));
    add("adDBTime", Variant::mkInt(135));
    add("adDBTimeStamp", Variant::mkInt(135));
    add("adFileTime", Variant::mkInt(64));
    add("adVariant", Variant::mkInt(12));
    add("adChar", Variant::mkInt(129));
    add("adVarChar", Variant::mkInt(200));
    add("adLongVarChar", Variant::mkInt(201));
    add("adWChar", Variant::mkInt(130));
    add("adVarWChar", Variant::mkInt(202));
    add("adLongVarWChar", Variant::mkInt(203));
    add("adBinary", Variant::mkInt(128));
    add("adVarBinary", Variant::mkInt(204));
    add("adLongVarBinary", Variant::mkInt(205));
    add("adBSTR", Variant::mkInt(8));
    // ADODB.CommandTypeEnum
    add("adCmdUnspecified", Variant::mkInt(-1));
    add("adCmdText", Variant::mkInt(1));
    add("adCmdTable", Variant::mkInt(2));
    add("adCmdStoredProc", Variant::mkInt(4));
    add("adCmdUnknown", Variant::mkInt(8));
    add("adCmdFile", Variant::mkInt(256));
    add("adCmdTableDirect", Variant::mkInt(512));
    // ADODB.CursorTypeEnum
    add("adOpenUnspecified", Variant::mkInt(-1));
    add("adOpenForwardOnly", Variant::mkInt(0));
    add("adOpenKeyset", Variant::mkInt(1));
    add("adOpenDynamic", Variant::mkInt(2));
    add("adOpenStatic", Variant::mkInt(3));
    // ADODB.LockTypeEnum
    add("adLockUnspecified", Variant::mkInt(-1));
    add("adLockReadOnly", Variant::mkInt(1));
    add("adLockPessimistic", Variant::mkInt(2));
    add("adLockOptimistic", Variant::mkInt(3));
    add("adLockBatchOptimistic", Variant::mkInt(4));
    // ADODB.ParameterDirectionEnum
    add("adParamUnknown", Variant::mkInt(0));
    add("adParamInput", Variant::mkInt(1));
    add("adParamOutput", Variant::mkInt(2));
    add("adParamInputOutput", Variant::mkInt(3));
    add("adParamReturnValue", Variant::mkInt(4));
    // ADODB.ExecuteOptionEnum
    add("adOptionUnspecified", Variant::mkInt(-1));
    add("adAsyncExecute", Variant::mkInt(16));
    add("adAsyncFetch", Variant::mkInt(32));
    add("adAsyncFetchNonBlocking", Variant::mkInt(64));
    add("adExecuteNoRecords", Variant::mkInt(128));
    add("adExecuteStream", Variant::mkInt(256));
    add("adExecuteRecord", Variant::mkInt(512));
    // ADODB.CursorLocationEnum
    add("adUseServer", Variant::mkInt(2));
    add("adUseClient", Variant::mkInt(3));
    // ADODB.PositionEnum / BookmarkEnum / EditModeEnum
    add("adPosUnknown", Variant::mkInt(-1));
    add("adPosBOF", Variant::mkInt(-2));
    add("adPosEOF", Variant::mkInt(-3));
    add("adBookmarkCurrent", Variant::mkInt(0));
    add("adBookmarkFirst", Variant::mkInt(1));
    add("adBookmarkLast", Variant::mkInt(2));
    add("adEditNone", Variant::mkInt(0));
    add("adEditInProgress", Variant::mkInt(1));
    add("adEditAdd", Variant::mkInt(2));
    add("adEditDelete", Variant::mkInt(4));
    // ADODB.StringFormatEnum
    add("adClipString", Variant::mkInt(2));
}

void registerBuiltins(Interp& ip) {
    auto& b = ip.builtins;
    auto reg = [&](const std::string& name, int minA, int maxA,
                   std::function<Variant(Interp&, std::vector<Variant>&)> fn) {
        Builtin bi; bi.minArgs = minA; bi.maxArgs = maxA; bi.fn = std::move(fn);
        b[lower(name)] = std::move(bi);
    };
    auto reg1 = [&](const std::string& name, int minA, int maxA, Variant (*fn)(std::vector<Variant>&)) {
        reg(name, minA, maxA, [fn](Interp&, std::vector<Variant>& a) { return fn(a); });
    };

    // Erl: line number of the statement currently executing
    reg("Erl", 0, 0, [](Interp& ip, std::vector<Variant>&) -> Variant {
        return Variant::mkLong(ip.cur ? ip.cur->curLine : 0);
    });

    // ---- strings ----
    reg1("Len", 1, 1, [](std::vector<Variant>& a) -> Variant {
        if (A(a,0).t == VT::Array) throwErr(13, errDesc(13));
        return Variant::mkLong((long long)cpLen(toStr(A(a, 0))));
    });
    reg1("LenB", 1, 1, [](std::vector<Variant>& a) -> Variant {
        return Variant::mkLong((long long)toStr(A(a, 0)).size());
    });
    reg1("Left", 2, 2, [](std::vector<Variant>& a) -> Variant {
        std::string s = toStr(A(a, 0));
        long long n = toInt64(A(a, 1));
        if (n < 0) throwErr(5, errDesc(5));
        return Variant::mkStr(cpSubstr(s, 0, (size_t)n));
    });
    reg1("LeftB", 2, 2, [](std::vector<Variant>& a) -> Variant {
        std::string s = toStr(A(a, 0));
        long long n = toInt64(A(a, 1));
        if (n < 0) throwErr(5, errDesc(5));
        return Variant::mkStr(s.substr(0, (size_t)n));
    });
    reg1("Right", 2, 2, [](std::vector<Variant>& a) -> Variant {
        std::string s = toStr(A(a, 0));
        long long n = toInt64(A(a, 1));
        if (n < 0) throwErr(5, errDesc(5));
        size_t len = cpLen(s);
        if ((size_t)n >= len) return Variant::mkStr(s);
        return Variant::mkStr(cpSubstr(s, len - (size_t)n, (size_t)n));
    });
    reg1("RightB", 2, 2, [](std::vector<Variant>& a) -> Variant {
        std::string s = toStr(A(a, 0));
        long long n = toInt64(A(a, 1));
        if (n < 0) throwErr(5, errDesc(5));
        if ((size_t)n >= s.size()) return Variant::mkStr(s);
        return Variant::mkStr(s.substr(s.size() - (size_t)n));
    });
    reg1("Mid", 2, 3, [](std::vector<Variant>& a) -> Variant {
        std::string s = toStr(A(a, 0));
        long long start = toInt64(A(a, 1));
        if (start < 1) throwErr(5, errDesc(5));
        size_t len = a.size() >= 3 ? (size_t)toInt64(A(a, 2)) : cpLen(s);
        if (a.size() >= 3 && toInt64(A(a, 2)) < 0) throwErr(5, errDesc(5));
        if ((size_t)start > cpLen(s)) return Variant::mkStr("");
        return Variant::mkStr(cpSubstr(s, (size_t)start - 1, len));
    });
    reg1("MidB", 2, 3, [](std::vector<Variant>& a) -> Variant {
        std::string s = toStr(A(a, 0));
        long long start = toInt64(A(a, 1));
        if (start < 1) throwErr(5, errDesc(5));
        size_t len = a.size() >= 3 ? (size_t)toInt64(A(a, 2)) : s.size();
        if ((size_t)start > s.size()) return Variant::mkStr("");
        return Variant::mkStr(s.substr((size_t)start - 1, len));
    });
    reg1("LCase", 1, 1, [](std::vector<Variant>& a) -> Variant { return Variant::mkStr(asciiCase(toStr(A(a, 0)), false)); });
    reg1("UCase", 1, 1, [](std::vector<Variant>& a) -> Variant { return Variant::mkStr(asciiCase(toStr(A(a, 0)), true)); });
    reg1("Trim", 1, 1, [](std::vector<Variant>& a) -> Variant { return Variant::mkStr(trimVb(toStr(A(a, 0)))); });
    reg1("LTrim", 1, 1, [](std::vector<Variant>& a) -> Variant { return Variant::mkStr(trimL(toStr(A(a, 0)))); });
    reg1("RTrim", 1, 1, [](std::vector<Variant>& a) -> Variant { return Variant::mkStr(trimR(toStr(A(a, 0)))); });
    reg1("Space", 1, 1, [](std::vector<Variant>& a) -> Variant {
        long long n = toInt64(A(a, 0));
        if (n < 0) throwErr(5, errDesc(5));
        if (n > (1LL << 28)) throwErr(14, errDesc(14)); // out of string space
        return Variant::mkStr(std::string((size_t)n, ' '));
    });
    reg1("String", 2, 2, [](std::vector<Variant>& a) -> Variant {
        long long n = toInt64(A(a, 0));
        if (n < 0) throwErr(5, errDesc(5));
        if (n > (1LL << 28)) throwErr(14, errDesc(14)); // out of string space
        const Variant& c = A(a, 1);
        std::string one;
        if (c.isNum()) one = cpToUtf8((unsigned)(toInt64(c) & 0xFFFF));
        else one = cpSubstr(toStr(c), 0, 1);
        if (one.empty()) throwErr(5, errDesc(5));
        std::string out;
        for (long long i = 0; i < n; i++) out += one;
        return Variant::mkStr(out);
    });
    reg1("StrComp", 2, 3, [](std::vector<Variant>& a) -> Variant {
        const Variant& x = A(a, 0), y = A(a, 1);
        if (x.isNull() || y.isNull()) return Variant::mkNull();
        int cmp = a.size() >= 3 ? (int)toInt64(A(a, 2)) : 0;
        std::string xs = toStr(x), ys = toStr(y);
        if (cmp == 1) { xs = asciiCase(xs, false); ys = asciiCase(ys, false); }
        return Variant::mkInt(xs < ys ? -1 : (xs > ys ? 1 : 0));
    });
    reg1("InStr", 2, 4, [](std::vector<Variant>& a) -> Variant {
        long long start = 1; std::string s1, s2; int cmp = 0;
        if (a.size() == 4) { start = toInt64(A(a, 0)); s1 = toStr(A(a, 1)); s2 = toStr(A(a, 2)); cmp = (int)toInt64(A(a, 3)); }
        else if (a.size() == 3 && a[0].isNum()) { start = toInt64(A(a, 0)); s1 = toStr(A(a, 1)); s2 = toStr(A(a, 2)); }
        else { s1 = toStr(A(a, 0)); s2 = toStr(A(a, 1)); if (a.size() >= 3) cmp = (int)toInt64(A(a, 2)); }
        if (start < 1) throwErr(5, errDesc(5));
        auto h = cpv(s1, cmp == 1);
        auto nd = cpv(s2, cmp == 1);
        if (nd.empty()) return Variant::mkLong(start <= (long long)h.size() + 1 ? start : 0);
        if ((size_t)start > h.size()) return Variant::mkLong(0);
        for (size_t i = (size_t)start - 1; i + nd.size() <= h.size(); i++) {
            bool ok = true;
            for (size_t j = 0; j < nd.size(); j++) if (h[i + j] != nd[j]) { ok = false; break; }
            if (ok) return Variant::mkLong((long long)i + 1);
        }
        return Variant::mkLong(0);
    });
    reg1("InStrRev", 2, 4, [](std::vector<Variant>& a) -> Variant {
        std::string s1 = toStr(A(a, 0)), s2 = toStr(A(a, 1));
        long long start = -1; int cmp = 0;
        if (a.size() >= 3) start = toInt64(A(a, 2));
        if (a.size() >= 4) cmp = (int)toInt64(A(a, 3));
        auto h = cpv(s1, cmp == 1);
        auto nd = cpv(s2, cmp == 1);
        if (nd.empty()) return Variant::mkLong((long long)h.size());
        long long lim = start < 1 ? (long long)h.size() : start - 1; // last candidate start (0-based)
        if (lim < 0) return Variant::mkLong(0);
        for (long long i = lim; i >= 0; i--) {
            if ((size_t)i + nd.size() > h.size()) continue;
            bool ok = true;
            for (size_t j = 0; j < nd.size(); j++) if (h[(size_t)i + j] != nd[j]) { ok = false; break; }
            if (ok) return Variant::mkLong(i + 1);
        }
        return Variant::mkLong(0);
    });
    reg1("Replace", 3, 6, [](std::vector<Variant>& a) -> Variant {
        std::string s = toStr(A(a, 0)), f = toStr(A(a, 1)), r = toStr(A(a, 2));
        long long start = a.size() >= 4 ? toInt64(A(a, 3)) : 1;
        long long count = a.size() >= 5 ? toInt64(A(a, 4)) : -1;
        int cmp = a.size() >= 6 ? (int)toInt64(A(a, 5)) : 0;
        if (start < 1) throwErr(5, errDesc(5));
        if (f.empty()) return Variant::mkStr(s);
        std::string h = cmp == 1 ? asciiCase(s, false) : s;
        std::string nd = cmp == 1 ? asciiCase(f, false) : f;
        // convert start (1-based char) to byte offset
        size_t byteOff = 0, cp = 0;
        while (byteOff < h.size() && cp < (size_t)start - 1) { size_t adv; cpAt(h, byteOff, &adv); byteOff += adv; cp++; }
        std::string head = s.substr(0, byteOff);
        std::string rest = s.substr(byteOff);
        std::string rh = cmp == 1 ? asciiCase(rest, false) : rest;
        std::string out = head;
        size_t pos = 0;
        long long replaced = 0;
        while (true) {
            size_t p = rh.find(nd, pos);
            if (p == std::string::npos) { out += rest.substr(pos); break; }
            out += rest.substr(pos, p - pos) + r;
            pos = p + nd.size();
            replaced++;
            if (count >= 0 && replaced >= count) { out += rest.substr(pos); break; }
        }
        return Variant::mkStr(out);
    });
    reg1("Split", 1, 4, [](std::vector<Variant>& a) -> Variant {
        std::string s = toStr(A(a, 0));
        std::string delim = a.size() >= 2 ? toStr(A(a, 1)) : " ";
        long long limit = a.size() >= 3 ? toInt64(A(a, 2)) : -1;
        int cmp = a.size() >= 4 ? (int)toInt64(A(a, 3)) : 0;
        if (delim.empty()) {
            // split into single chars
            std::vector<Variant> items;
            size_t i = 0;
            while (i < s.size()) { size_t adv; unsigned c = cpAt(s, i, &adv); items.push_back(Variant::mkStr(cpToUtf8(c))); i += adv; }
            return Variant::mkArr(makeStrArray(std::move(items)));
        }
        std::vector<Variant> items;
        std::string h = cmp == 1 ? asciiCase(s, false) : s;
        std::string nd = cmp == 1 ? asciiCase(delim, false) : delim;
        size_t pos = 0, found = 0;
        while (limit < 0 || (long long)found < limit - 1) {
            size_t p = h.find(nd, pos);
            if (p == std::string::npos) break;
            items.push_back(Variant::mkStr(s.substr(pos, p - pos)));
            pos = p + nd.size();
            found++;
        }
        items.push_back(Variant::mkStr(s.substr(pos)));
        return Variant::mkArr(makeStrArray(std::move(items)));
    });
    reg1("Join", 1, 2, [](std::vector<Variant>& a) -> Variant {
        const Variant& arr = A(a, 0);
        if (arr.t != VT::Array || !arr.arr) throwErr(13, errDesc(13));
        std::string delim = a.size() >= 2 ? toStr(A(a, 1)) : " ";
        std::string out;
        bool first = true;
        for (auto& v : arr.arr->data) {
            if (!first) out += delim;
            out += toStr(v);
            first = false;
        }
        return Variant::mkStr(out);
    });
    reg1("StrReverse", 1, 1, [](std::vector<Variant>& a) -> Variant {
        std::string s = toStr(A(a, 0));
        std::string out;
        size_t i = s.size();
        while (i > 0) {
            // find start of previous code point
            size_t j = i - 1;
            while (j > 0 && ((unsigned char)s[j] & 0xC0) == 0x80) j--;
            size_t adv = 0;
            unsigned c = cpAt(s, j, &adv);
            out += cpToUtf8(c);
            i = j;
        }
        return Variant::mkStr(out);
    });
    reg1("Asc", 1, 1, [](std::vector<Variant>& a) -> Variant {
        std::string s = toStr(A(a, 0));
        if (s.empty()) throwErr(5, errDesc(5));
        return Variant::mkLong((long long)cpAt(s, 0));
    });
    reg1("AscW", 1, 1, [](std::vector<Variant>& a) -> Variant {
        std::string s = toStr(A(a, 0));
        if (s.empty()) throwErr(5, errDesc(5));
        return Variant::mkLong((long long)cpAt(s, 0));
    });
    reg1("Chr", 1, 1, [](std::vector<Variant>& a) -> Variant {
        long long n = toInt64(A(a, 0));
        if (n < 0 || n > 0x10FFFF) throwErr(5, errDesc(5));
        return Variant::mkStr(cpToUtf8((unsigned)n));
    });
    reg1("ChrW", 1, 1, [](std::vector<Variant>& a) -> Variant {
        long long n = toInt64(A(a, 0)) & 0xFFFF;
        return Variant::mkStr(cpToUtf8((unsigned)n));
    });
    // Hex/Oct: an Integer prints its 16-bit pattern (Hex(-1) = "FFFF"); any
    // other value is rounded to a Long first (error 6 outside the Long range)
    // and prints its 32-bit pattern. Null gives Null.
    reg1("Hex", 1, 1, [](std::vector<Variant>& a) -> Variant {
        const Variant& v = A(a, 0);
        if (v.isNull()) return Variant::mkNull();
        uint32_t bits = v.t == VT::Int ? (uint16_t)v.ll : (uint32_t)toLongValue(v);
        char buf[32];
        std::snprintf(buf, sizeof buf, "%X", (unsigned)bits);
        return Variant::mkStr(buf);
    });
    reg1("Oct", 1, 1, [](std::vector<Variant>& a) -> Variant {
        const Variant& v = A(a, 0);
        if (v.isNull()) return Variant::mkNull();
        uint32_t bits = v.t == VT::Int ? (uint16_t)v.ll : (uint32_t)toLongValue(v);
        char buf[32];
        std::snprintf(buf, sizeof buf, "%o", (unsigned)bits);
        return Variant::mkStr(buf);
    });

    // ---- conversion ----
    reg1("CStr", 1, 1, [](std::vector<Variant>& a) -> Variant { return Variant::mkStr(toStr(A(a, 0))); });
    reg1("CInt", 1, 1, [](std::vector<Variant>& a) -> Variant {
        double d = toDouble(A(a, 0));
        long long v = bankerRound(d);
        if (v < -32768 || v > 32767) throwErr(6, errDesc(6));
        return Variant::mkInt(v);
    });
    reg1("CLng", 1, 1, [](std::vector<Variant>& a) -> Variant {
        double d = toDouble(A(a, 0));
        long long v = bankerRound(d);
        if (v < -2147483648LL || v > 2147483647LL) throwErr(6, errDesc(6));
        return Variant::mkLong(v);
    });
    reg1("CByte", 1, 1, [](std::vector<Variant>& a) -> Variant {
        return Variant::mkByte(toByteValue(A(a, 0)));     // 0..255, banker's rounding
    });
    reg1("CDbl", 1, 1, [](std::vector<Variant>& a) -> Variant { return Variant::mkDouble(toDouble(A(a, 0))); });
    reg1("CSng", 1, 1, [](std::vector<Variant>& a) -> Variant {
        return Variant::mkSingle(toSingleValue(A(a, 0))); // error 6 beyond the float range
    });
    reg1("CCur", 1, 1, [](std::vector<Variant>& a) -> Variant {
        return Variant::mkCur(toCurrencyScaled(A(a, 0))); // 4 places, banker's rounding
    });
    reg1("CBool", 1, 1, [](std::vector<Variant>& a) -> Variant { return Variant::mkBool(toBool(A(a, 0))); });
    reg1("CDate", 1, 1, [](std::vector<Variant>& a) -> Variant {
        const Variant& v = A(a, 0);
        if (v.t == VT::Date) return v;
        if (v.t == VT::String) {
            double s;
            if (parseDateString(v.s, s)) return Variant::mkDate(s);
            throwErr(13, errDesc(13));
        }
        if (v.isNum()) return Variant::mkDate(toDouble(v));
        throwErr(13, errDesc(13));
    });
    reg1("Val", 1, 1, [](std::vector<Variant>& a) -> Variant {
        std::string s = toStr(A(a, 0));
        size_t i = 0;
        while (i < s.size() && ws(s[i])) i++;
        size_t start = i;
        if (i < s.size() && (s[i] == '+' || s[i] == '-')) i++;
        bool any = false;
        while (i < s.size() && (std::isdigit((unsigned char)s[i]) || s[i] == '.')) { if (std::isdigit((unsigned char)s[i])) any = true; i++; }
        if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
            size_t j = i + 1;
            if (j < s.size() && (s[j] == '+' || s[j] == '-')) j++;
            if (j < s.size() && std::isdigit((unsigned char)s[j])) {
                while (j < s.size() && std::isdigit((unsigned char)s[j])) j++;
                i = j;
                any = true;
            }
        }
        if (!any) return Variant::mkDouble(0);
        return Variant::mkDouble(std::strtod(s.substr(start, i - start).c_str(), nullptr));
    });
    // Int, Fix, Abs and Round keep the numeric subtype of their argument
    // (Byte, Integer, Long, Single, Double, Currency); Boolean and Empty give an
    // Integer, a String a Double, and Null gives Null.
    reg1("Int", 1, 1, [](std::vector<Variant>& a) -> Variant {
        return roundKeepingType(A(a, 0), [](double d) { return std::floor(d); }, 0);
    });
    reg1("Fix", 1, 1, [](std::vector<Variant>& a) -> Variant {
        return roundKeepingType(A(a, 0), [](double d) { return std::trunc(d); }, 1);
    });
    reg1("Abs", 1, 1, [](std::vector<Variant>& a) -> Variant {
        const Variant& v = A(a, 0);
        switch (v.t) {
            case VT::Null:  return Variant::mkNull();
            case VT::Byte:  return v;
            case VT::Empty: return Variant::mkI2(0);
            case VT::Bool:  return Variant::mkI2(v.b ? 1 : 0);
            case VT::Int: {
                long long r = v.ll < 0 ? -v.ll : v.ll;   // Abs(-32768) is a Long
                return r > 32767 ? Variant::mkLong(r) : Variant::mkI2(r);
            }
            case VT::Long: {
                long long r = v.ll < 0 ? -v.ll : v.ll;   // Abs(-2147483648) is a Double
                return r > 2147483647LL ? Variant::mkDouble((double)r) : Variant::mkLong(r);
            }
            case VT::Single: return Variant::mkSingle(std::fabs(v.d));
            case VT::Currency:
                if (v.ll == LLONG_MIN) throwErr(6, errDesc(6));
                return Variant::mkCur(v.ll < 0 ? -v.ll : v.ll);
            default: return Variant::mkDouble(std::fabs(toDouble(v)));
        }
    });
    reg1("Sgn", 1, 1, [](std::vector<Variant>& a) -> Variant {
        double d = toDouble(A(a, 0));
        return Variant::mkInt(d > 0 ? 1 : (d < 0 ? -1 : 0));
    });
    reg1("Sqr", 1, 1, [](std::vector<Variant>& a) -> Variant {
        double d = toDouble(A(a, 0));
        if (d < 0) throwErr(5, errDesc(5));
        return Variant::mkDouble(std::sqrt(d));
    });
    reg1("Exp", 1, 1, [](std::vector<Variant>& a) -> Variant { return Variant::mkDouble(std::exp(toDouble(A(a, 0)))); });
    reg1("Log", 1, 1, [](std::vector<Variant>& a) -> Variant {
        double d = toDouble(A(a, 0));
        if (d <= 0) throwErr(5, errDesc(5));
        return Variant::mkDouble(std::log(d));
    });
    reg1("Sin", 1, 1, [](std::vector<Variant>& a) -> Variant { return Variant::mkDouble(std::sin(toDouble(A(a, 0)))); });
    reg1("Cos", 1, 1, [](std::vector<Variant>& a) -> Variant { return Variant::mkDouble(std::cos(toDouble(A(a, 0)))); });
    reg1("Tan", 1, 1, [](std::vector<Variant>& a) -> Variant { return Variant::mkDouble(std::tan(toDouble(A(a, 0)))); });
    reg1("Atn", 1, 1, [](std::vector<Variant>& a) -> Variant { return Variant::mkDouble(std::atan(toDouble(A(a, 0)))); });
    reg("Round", 1, 2, [](Interp&, std::vector<Variant>& a) -> Variant {
        const Variant& v = A(a, 0);
        long long digits = a.size() >= 2 ? toInt64(A(a, 1)) : 0;
        switch (v.t) {
            case VT::Null:  return Variant::mkNull();
            case VT::Byte: case VT::Int: case VT::Long: return v;
            case VT::Empty: return Variant::mkI2(0);
            case VT::Bool:  return Variant::mkI2(v.b ? -1 : 0);
            case VT::Currency: {
                if (digits >= 4) return v;
                long long q = 1;
                for (long long i = digits < 0 ? 0 : digits; i < 4; i++) q *= 10;
                // banker's rounding of the scaled value to a multiple of q
                long long m = v.ll < 0 ? -(v.ll / q) : v.ll / q, rem = v.ll % q;
                if (rem < 0) rem = -rem;
                if (rem * 2 > q || (rem * 2 == q && (m & 1))) m++;
                __int128 r = (__int128)(v.ll < 0 ? -m : m) * q;
                if (r > LLONG_MAX || r < LLONG_MIN) throwErr(6, errDesc(6));
                return Variant::mkCur((long long)r);
            }
            case VT::Single: {
                double scale = std::pow(10.0, (double)digits);
                double r = bankerRound(v.d * scale) / scale;
                return Variant::mkSingle(r == 0 ? 0 : r);
            }
            default: break;
        }
        double d = toDouble(v);
        double scale = std::pow(10.0, (double)digits);
        double r = bankerRound(d * scale) / scale;
        // normalize -0
        if (r == 0) r = 0;
        return Variant::mkDouble(r);
    });
    reg("Rnd", 0, 1, [](Interp& ip, std::vector<Variant>& a) -> Variant {
        if (a.size() >= 1) {
            double n = toDouble(A(a, 0));
            if (n < 0) ip.seedRng(n);
            return Variant::mkDouble(ip.rnd());
        }
        return Variant::mkDouble(ip.rnd());
    });
    reg1("IsNumeric", 1, 1, [](std::vector<Variant>& a) -> Variant {
        const Variant& v = A(a, 0);
        if (v.t == VT::String) {
            double d;
            return Variant::mkBool(parseNumericString(v.s, d));
        }
        if (v.isNumericSubtype()) return Variant::mkBool(true);
        return Variant::mkBool(false);
    });
    reg1("IsDate", 1, 1, [](std::vector<Variant>& a) -> Variant {
        const Variant& v = A(a, 0);
        if (v.t == VT::Date) return Variant::mkBool(true);
        if (v.t == VT::String) {
            double d;
            return Variant::mkBool(parseDateString(v.s, d));
        }
        return Variant::mkBool(false);
    });
    reg1("IsEmpty", 1, 1, [](std::vector<Variant>& a) -> Variant { return Variant::mkBool(A(a, 0).isEmpty()); });
    reg1("IsNull", 1, 1, [](std::vector<Variant>& a) -> Variant { return Variant::mkBool(A(a, 0).isNull()); });
    reg1("IsObject", 1, 1, [](std::vector<Variant>& a) -> Variant {
        const Variant& v = A(a, 0);
        return Variant::mkBool(v.t == VT::Object && v.obj != nullptr);
    });
    reg1("IsArray", 1, 1, [](std::vector<Variant>& a) -> Variant { return Variant::mkBool(A(a, 0).t == VT::Array); });
    reg1("VarType", 1, 1, [](std::vector<Variant>& a) -> Variant { return Variant::mkInt(varTypeOf(A(a, 0))); });
    reg1("TypeName", 1, 1, [](std::vector<Variant>& a) -> Variant { return Variant::mkStr(typeNameOf(A(a, 0))); });
    reg1("Choose", 2, 256, [](std::vector<Variant>& a) -> Variant {
        long long idx = toInt64(A(a, 0));
        if (idx < 1 || (size_t)idx >= a.size()) return Variant::mkNull();
        return a[(size_t)idx];
    });
    reg1("Switch", 2, 256, [](std::vector<Variant>& a) -> Variant {
        for (size_t i = 0; i + 1 < a.size(); i += 2)
            if (toBool(a[i])) return a[i + 1];
        return Variant::mkNull();
    });
    reg1("IIf", 3, 3, [](std::vector<Variant>& a) -> Variant { return toBool(A(a, 0)) ? A(a, 1) : A(a, 2); });
    reg("Array", 0, 256, [](Interp&, std::vector<Variant>& a) -> Variant {
        auto arr = std::make_shared<VArray>();
        arr->base = 0;
        arr->dims = {(long long)a.size()};
        arr->data.assign(a.begin(), a.end());
        arr->allocated = true;
        return Variant::mkArr(arr);
    });
    reg1("UBound", 1, 2, [](std::vector<Variant>& a) -> Variant {
        const Variant& v = A(a, 0);
        if (v.t != VT::Array || !v.arr) throwErr(13, errDesc(13));
        if (!v.arr->allocated) throwErr(9, errDesc(9));
        long long dim = a.size() >= 2 ? toInt64(A(a, 1)) : 1; // no dimension = the first
        if (dim < 1 || (size_t)dim > v.arr->dims.size()) throwErr(9, errDesc(9));
        return Variant::mkLong(v.arr->base + v.arr->dims[(size_t)dim - 1] - 1);
    });
    reg1("LBound", 1, 2, [](std::vector<Variant>& a) -> Variant {
        const Variant& v = A(a, 0);
        if (v.t != VT::Array || !v.arr) throwErr(13, errDesc(13));
        if (!v.arr->allocated) throwErr(9, errDesc(9));
        long long dim = a.size() >= 2 ? toInt64(A(a, 1)) : 1;
        if (dim < 1 || (size_t)dim > v.arr->dims.size()) throwErr(9, errDesc(9));
        return Variant::mkLong(v.arr->base);
    });
    reg1("RGB", 3, 3, [](std::vector<Variant>& a) -> Variant {
        long long r = toInt64(A(a, 0)) & 0xFF, g = toInt64(A(a, 1)) & 0xFF, bl = toInt64(A(a, 2)) & 0xFF;
        return Variant::mkLong(r | (g << 8) | (bl << 16));
    });

    // ---- dates ----
    reg1("Now", 0, 0, [](std::vector<Variant>&) -> Variant { return Variant::mkDate(nowSerial()); });
    reg1("Date", 0, 0, [](std::vector<Variant>&) -> Variant { return Variant::mkDate(std::floor(nowSerial())); });
    reg1("Time", 0, 0, [](std::vector<Variant>&) -> Variant { return Variant::mkDate(nowSerial() - std::floor(nowSerial())); });
    reg1("Timer", 0, 0, [](std::vector<Variant>&) -> Variant {
        std::time_t t = std::time(nullptr);
        std::tm tmv; localtime_r(&t, &tmv);
        return Variant::mkDouble(tmv.tm_hour * 3600 + tmv.tm_min * 60 + tmv.tm_sec);
    });
    auto mkDatePart = [&](const std::string& unit) {
        reg(unit == "yyyy" ? "Year" : unit == "m" ? "Month" : unit == "d" ? "Day" :
            unit == "h" ? "Hour" : unit == "n" ? "Minute" : "Second", 1, 1,
            [unit](Interp&, std::vector<Variant>& a) -> Variant { return datePartImpl(unit, dateArg(a, 0)); });
    };
    mkDatePart("yyyy");
    mkDatePart("m");
    mkDatePart("d");
    mkDatePart("h");
    mkDatePart("n");
    mkDatePart("s");
    reg("Weekday", 1, 2, [](Interp&, std::vector<Variant>& a) -> Variant {
        double d = dateArg(a, 0);
        long long first = a.size() >= 2 ? toInt64(A(a, 1)) : 1;
        if (first < 1 || first > 7) throwErr(5, errDesc(5));
        int wd = weekdayOf(d); // 1=Sun..7=Sat
        return Variant::mkInt(((wd - (int)first) % 7 + 7) % 7 + 1);
    });
    reg("DateSerial", 3, 3, [](Interp&, std::vector<Variant>& a) -> Variant {
        long long y = toInt64(A(a, 0));
        long long m = toInt64(A(a, 1)), d = toInt64(A(a, 2));
        // normalize month overflow
        y += (m - 1) / 12;
        m = (m - 1) % 12 + 1;
        if (m <= 0) { m += 12; y--; }
        long long base = daysFromCivil(y, (unsigned)m, 1) + VB_EPOCH_OFF;
        return Variant::mkDate((double)(base + (d - 1)));
    });
    reg("TimeSerial", 3, 3, [](Interp&, std::vector<Variant>& a) -> Variant {
        long long h = toInt64(A(a, 0)), m = toInt64(A(a, 1)), s = toInt64(A(a, 2));
        return Variant::mkDate((h * 3600.0 + m * 60.0 + s) / 86400.0);
    });
    reg1("DateValue", 1, 1, [](std::vector<Variant>& a) -> Variant {
        const Variant& v = A(a, 0);
        if (v.t == VT::Date) return Variant::mkDate(std::floor(v.d));
        double s;
        if (!parseDateString(toStr(v), s)) throwErr(13, errDesc(13));
        return Variant::mkDate(std::floor(s));
    });
    reg1("TimeValue", 1, 1, [](std::vector<Variant>& a) -> Variant {
        const Variant& v = A(a, 0);
        if (v.t == VT::Date) return Variant::mkDate(v.d - std::floor(v.d));
        double s;
        if (!parseDateString(toStr(v), s)) throwErr(13, errDesc(13));
        return Variant::mkDate(s - std::floor(s));
    });
    reg("DateAdd", 3, 3, [](Interp&, std::vector<Variant>& a) -> Variant {
        std::string unit = lower(toStr(A(a, 0)));
        long long n = normInterval(toDouble(A(a, 1)));
        double d = dateArg(a, 2);
        return dateAddImpl(unit, n, d);
    });
    reg("DateDiff", 3, 5, [](Interp&, std::vector<Variant>& a) -> Variant {
        std::string unit = lower(toStr(A(a, 0)));
        double d1 = dateArg(a, 1), d2 = dateArg(a, 2);
        return dateDiffImpl(unit, d1, d2);
    });
    reg("DatePart", 2, 4, [](Interp&, std::vector<Variant>& a) -> Variant {
        std::string unit = lower(toStr(A(a, 0)));
        double d = dateArg(a, 1);
        return datePartImpl(unit, d);
    });
    reg("MonthName", 1, 2, [](Interp&, std::vector<Variant>& a) -> Variant {
        static const char* full[] = {"", "January","February","March","April","May","June","July","August","September","October","November","December"};
        static const char* ab[] = {"", "Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};
        long long m = toInt64(A(a, 0));
        bool abbrev = a.size() >= 2 ? toBool(A(a, 1)) : false;
        if (m < 1 || m > 12) return Variant::mkStr("");
        return Variant::mkStr(abbrev ? ab[m] : full[m]);
    });
    reg("WeekdayName", 1, 3, [](Interp&, std::vector<Variant>& a) -> Variant {
        static const char* full[] = {"", "Sunday","Monday","Tuesday","Wednesday","Thursday","Friday","Saturday"};
        static const char* ab[] = {"", "Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
        long long w = toInt64(A(a, 0));
        bool abbrev = a.size() >= 2 ? toBool(A(a, 1)) : false;
        long long first = a.size() >= 3 ? toInt64(A(a, 2)) : 1;
        if (w < 1 || w > 7) return Variant::mkStr("");
        long long idx = ((w + (int)first - 2) % 7 + 7) % 7 + 1;
        return Variant::mkStr(abbrev ? ab[idx] : full[idx]);
    });
    // FormatDateTime(date[, format]). Formats are the en-US Windows defaults,
    // like the rest of mscript's date output (CStr, MonthName, ...):
    //   vbGeneralDate 0  date and/or time, whichever parts are non-zero
    //   vbLongDate    1  "dddd, MMMM d, yyyy"   e.g. Friday, January 5, 2024
    //   vbShortDate   2  "M/d/yyyy"             e.g. 1/5/2024
    //   vbLongTime    3  "h:mm:ss tt"           e.g. 1:04:09 PM
    //   vbShortTime   4  24-hour "HH:mm"        e.g. 13:04
    // Any other format raises error 5; a Null date raises 94.
    reg("FormatDateTime", 1, 2, [](Interp&, std::vector<Variant>& a) -> Variant {
        if (A(a, 0).isNull()) throwErr(94, errDesc(94));
        long long fmt = a.size() >= 2 && !A(a, 1).isEmpty() ? toInt64(A(a, 1)) : 0;
        if (fmt < 0 || fmt > 4) throwErr(5, errDesc(5));
        double d = dateArg(a, 0);
        long long y; unsigned m, dd; int hh, mm, ss;
        splitSerial(d, y, m, dd, hh, mm, ss);
        static const char* months[] = {"", "January","February","March","April","May","June","July","August","September","October","November","December"};
        static const char* days[] = {"", "Sunday","Monday","Tuesday","Wednesday","Thursday","Friday","Saturday"};
        switch (fmt) {
            case 1: { // vbLongDate
                int wd = weekdayOf(d);
                return Variant::mkStr(std::string(days[wd]) + ", " + months[m] + " " + std::to_string(dd) + ", " + std::to_string(y));
            }
            case 2: return Variant::mkStr(formatDate(d));       // vbShortDate
            case 3: return Variant::mkStr(formatTimeOfDay(d));  // vbLongTime
            case 4: {                                           // vbShortTime
                char buf[16];
                std::snprintf(buf, sizeof buf, "%02d:%02d", hh, mm);
                return Variant::mkStr(buf);
            }
            default: return Variant::mkStr(formatDateTimeV(d)); // vbGeneralDate
        }
    });
    reg("Format", 1, 2, [](Interp&, std::vector<Variant>& a) -> Variant {
        const Variant& v = A(a, 0);
        std::string fmt = a.size() >= 2 ? toStr(A(a, 1)) : "";
        std::string lf = lower(fmt);
        if (v.t == VT::Date || lf == "short date" || lf == "long date" || lf == "short time" || lf == "long time" || lf == "general date") {
            double d = v.t == VT::Date ? v.d : dateArg(a, 0);
            if (lf == "long date") return Variant::mkStr([&]{ long long y; unsigned m, dd; int hh, mm, ss; splitSerial(d, y, m, dd, hh, mm, ss);
                static const char* months[] = {"", "January","February","March","April","May","June","July","August","September","October","November","December"};
                static const char* days[] = {"", "Sunday","Monday","Tuesday","Wednesday","Thursday","Friday","Saturday"};
                return std::string(days[weekdayOf(d)]) + ", " + months[m] + " " + std::to_string(dd) + ", " + std::to_string(y); }());
            if (lf == "short date") return Variant::mkStr(formatDate(d));
            if (lf == "long time") return Variant::mkStr(formatTimeOfDay(d));
            if (lf == "short time") { long long y; unsigned m, dd; int hh, mm, ss; splitSerial(d, y, m, dd, hh, mm, ss); char buf[32]; std::snprintf(buf, sizeof buf, "%d:%02d %s", hh % 12 == 0 ? 12 : hh % 12, mm, hh < 12 ? "AM" : "PM"); return Variant::mkStr(buf); }
            return Variant::mkStr(formatDateTimeV(d));
        }
        return Variant::mkStr(fmtNum(toDouble(v), fmt));
    });
    reg("FormatNumber", 1, 5, [](Interp&, std::vector<Variant>& a) -> Variant {
        double v = toDouble(A(a, 0));
        long long digits = a.size() >= 2 ? toInt64(A(a, 1)) : -1;
        std::string pat;
        if (digits >= 0) pat = "0." + std::string((size_t)digits, '0');
        return Variant::mkStr(fmtNum(v, pat));
    });
    reg("FormatPercent", 1, 5, [](Interp&, std::vector<Variant>& a) -> Variant {
        double v = toDouble(A(a, 0));
        long long digits = a.size() >= 2 ? toInt64(A(a, 1)) : -1;
        std::string pat = "%";
        if (digits >= 0) pat = "0." + std::string((size_t)digits, '0') + "%";
        return Variant::mkStr(fmtNum(v, pat));
    });

    // ---- host interaction ----
    reg("MsgBox", 1, 5, [](Interp& ip, std::vector<Variant>& a) -> Variant {
        std::string msg = toStrLenient(A(a, 0));
        ip.printOut(msg + "\n");
        return Variant::mkInt(1); // vbOK
    });
    reg("InputBox", 1, 7, [](Interp& ip, std::vector<Variant>& a) -> Variant {
        std::string prompt = a.size() >= 1 ? toStrLenient(A(a, 0)) : "";
        std::string def = a.size() >= 3 ? toStrLenient(A(a, 2)) : "";
        ip.printOut(prompt);
        std::string line;
        if (!std::getline(std::cin, line)) return Variant::mkStr(def);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        return Variant::mkStr(line);
    });
    reg("CreateObject", 1, 2, [](Interp& ip, std::vector<Variant>& a) -> Variant {
        std::string progId = toStr(A(a, 0));
        auto obj = createHostObject(ip, progId);
        if (!obj) throwErr(429, errDesc(429));
        return Variant::mkObj(obj);
    });
    reg1("GetObject", 1, 2, [](std::vector<Variant>&) -> Variant { throwErr(429, errDesc(429)); });
    reg1("ScriptEngine", 0, 0, [](std::vector<Variant>&) -> Variant { return Variant::mkStr("VBScript"); });
    reg1("ScriptEngineMajorVersion", 0, 0, [](std::vector<Variant>&) -> Variant { return Variant::mkInt(5); });
    reg1("ScriptEngineMinorVersion", 0, 0, [](std::vector<Variant>&) -> Variant { return Variant::mkInt(8); });
    reg1("ScriptEngineBuildVersion", 0, 0, [](std::vector<Variant>&) -> Variant { return Variant::mkInt(40000); });
    reg("Eval", 1, 1, [](Interp& ip, std::vector<Variant>& a) -> Variant {
        return ip.evalString(toStr(A(a, 0)));
    });
    reg1("GetRef", 1, 1, [](std::vector<Variant>&) -> Variant { throwErr(450, "GetRef is not supported"); });
    reg1("LoadPicture", 1, 1, [](std::vector<Variant>&) -> Variant { throwErr(453, "LoadPicture is not supported"); });
}
