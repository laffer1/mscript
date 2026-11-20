// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 Lucas Holt. All rights reserved.

// variant.h — VBScript Variant value model, coercions, operators, arrays, dates.
#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

enum class VT { Empty, Null, Bool, Int, Long, Double, Date, String, Object, Array };

struct VBObject;
struct VArray;

struct Variant {
    VT t = VT::Empty;
    bool b = false;                 // Bool
    long long ll = 0;               // Int / Long
    double d = 0.0;                 // Double / Date (days since 1899-12-30)
    std::string s;                  // String
    std::shared_ptr<VBObject> obj;  // Object (null obj == Nothing)
    std::shared_ptr<VArray> arr;    // Array

    Variant() = default;
    static Variant mkBool(bool x)   { Variant v; v.t = VT::Bool;   v.b = x; return v; }
    static Variant mkInt(long long x) {
        Variant v; v.ll = x;
        // VBScript literal subtype: int16 -> Integer, int32 -> Long, else Double
        if (x < -2147483648LL || x > 2147483647LL) { v.t = VT::Double; v.d = (double)x; }
        else v.t = (x >= -32768 && x <= 32767) ? VT::Int : VT::Long;
        return v;
    }
    static Variant mkLong(long long x) { Variant v; v.t = VT::Long; v.ll = x; return v; }
    static Variant mkDouble(double x)  { Variant v; v.t = VT::Double; v.d = x; return v; }
    static Variant mkDate(double x)    { Variant v; v.t = VT::Date;   v.d = x; return v; }
    static Variant mkStr(std::string x){ Variant v; v.t = VT::String; v.s = std::move(x); return v; }
    static Variant mkObj(std::shared_ptr<VBObject> o) { Variant v; v.t = VT::Object; v.obj = std::move(o); return v; }
    static Variant mkArr(std::shared_ptr<VArray> a)    { Variant v; v.t = VT::Array;  v.arr = std::move(a); return v; }
    static Variant mkNull()         { Variant v; v.t = VT::Null; return v; }

    bool isNull()  const { return t == VT::Null; }
    bool isEmpty() const { return t == VT::Empty; }
    bool isNum() const {
        return t == VT::Int || t == VT::Long || t == VT::Double ||
               t == VT::Bool || t == VT::Date || t == VT::Empty;
    }
    bool isIntegral() const { return t == VT::Int || t == VT::Long; }
};

// Runtime error carrying a VBScript error number (e.g. 13 = Type mismatch).
struct VBSError : std::runtime_error {
    int number;
    std::string source;
    std::string helpfile;
    int helpcontext = 0;
    int line = 0;
    VBSError(int num, const std::string& desc, std::string src = "",
             std::string hf = "", int hc = 0)
        : std::runtime_error(desc), number(num), source(std::move(src)),
          helpfile(std::move(hf)), helpcontext(hc) {}
};

[[noreturn]] void throwErr(int num, const std::string& desc);
std::string errDesc(int num);
int mapHRESULTToVB(uint32_t hres); // raw HRESULT -> Err.Number (VB) value

struct VArray {
    int base = 0;                    // Option Base
    std::vector<long long> dims;     // size per dimension
    std::deque<Variant> data;        // flat row-major storage (deque: stable refs)
    bool allocated = true;
    bool fixed = false;              // declared with explicit dims (ReDim -> error 10)

    long long totalSize() const;
    long long offsetOf(const std::vector<long long>& idx) const; // throws 9
    Variant& at(const std::vector<long long>& idx);
    const Variant& at(const std::vector<long long>& idx) const;
    void alloc(const std::vector<long long>& newDims, int newBase, bool preserve);
};

// VBScript arrays are values: storing one into a variable, an array element, a
// ByVal parameter, a class field or a collection stores a copy. Variants share
// their VArray through a shared_ptr, so ownArrays() gives v its own storage
// wherever another Variant still points at it, recursing into nested arrays.
// An array nothing else references (a function result, Array(), Split(), ...)
// is kept as it is, so temporaries are stored without being copied.
void ownArrays(Variant& v);

struct VBObject {
    virtual ~VBObject() = default;
    // All names arrive lowercased.
    virtual bool hasMember(const std::string&) const { return false; }
    virtual bool hasMethod(const std::string&) const { return false; }
    virtual Variant getMember(const std::string& name);
    virtual void setMember(const std::string& name, const Variant& v);
    virtual Variant callMember(const std::string& name, std::vector<Variant>& args);
    virtual Variant invoke(const std::vector<Variant>& args);              // obj(args)
    virtual void assignInvoke(const std::vector<Variant>& args, const Variant& v); // obj(args) = v
    virtual std::vector<Variant> enumItems();                               // For Each support
    virtual bool hasDefault() const { return false; }                       // default property
    virtual Variant getDefault() { throwErr(438, errDesc(438)); }
    virtual std::string objectKind() const { return "Object"; }
    virtual std::string toString();
};

// Generic object backed by property/method maps (used for WScript, Err, ...).
struct MapObject : VBObject {
    std::map<std::string, Variant> props;
    std::map<std::string, std::function<Variant(MapObject*, std::vector<Variant>&)>> methods;
    std::function<Variant(MapObject*, std::vector<Variant>&)> onInvoke;
    std::function<void(MapObject*, std::vector<Variant>&, const Variant&)> onAssignInvoke;
    std::function<std::vector<Variant>(MapObject*)> onEnum;
    std::string kind = "Object";

    bool hasMember(const std::string& n) const override {
        return props.count(n) || methods.count(n);
    }
    bool hasMethod(const std::string& n) const override {
        return methods.count(n) != 0;
    }
    Variant getMember(const std::string& n) override;
    void setMember(const std::string& n, const Variant& v) override;
    Variant callMember(const std::string& n, std::vector<Variant>& args) override;
    Variant invoke(const std::vector<Variant>& args) override;
    void assignInvoke(const std::vector<Variant>& args, const Variant& v) override;
    std::vector<Variant> enumItems() override;
    std::string objectKind() const override { return kind; }
};

// ---- helpers ----
std::string lower(std::string s);
std::string trim(const std::string& s);

std::string toStr(const Variant& v);        // strict: Null throws 94
std::string toStrLenient(const Variant& v); // Null -> "", used for host output
double toDouble(const Variant& v);          // Null throws 94, bad string throws 13
long long toInt64(const Variant& v);
bool toBool(const Variant& v);
bool parseNumericString(const std::string& s, double& out); // "&H.."/"&O.."/decimal; "" -> 0

long long bankerRound(double d);            // VB banker's rounding
std::string formatDouble(double d);         // VB-style number -> string

int varTypeOf(const Variant& v);            // vb* constants
std::string typeNameOf(const Variant& v);

// ---- date utils (serial = days since 1899-12-30, VB epoch) ----
// daysFromCivil/civilFromDays use the Unix epoch (1970-01-01); VB_EPOCH_OFF is the
// number of days from 1899-12-30 to 1970-01-01 (25569).
inline constexpr long long VB_EPOCH_OFF = 25569;
long long daysFromCivil(long long y, unsigned m, unsigned d);
void civilFromDays(long long z, long long& y, unsigned& m, unsigned& d);
unsigned daysInMonth(long long y, unsigned m);
double makeSerial(long long y, unsigned m, unsigned d, int hh, int mm, int ss);
bool parseDateString(const std::string& s, double& outSerial);
int weekdayOf(double serial);               // 1=Sunday .. 7=Saturday
std::string formatDate(double serial);      // M/D/YYYY
std::string formatTimeOfDay(double serial); // h:mm:ss AM/PM
std::string formatDateTimeV(double serial); // M/D/YYYY h:mm:ss AM/PM

// ---- operators (Variant semantics) ----
Variant opAdd(const Variant& a, const Variant& b);     // + : numeric, or concat if both strings
Variant opConcat(const Variant& a, const Variant& b);  // & : always concat; Null only if both are Null
Variant opSub(const Variant& a, const Variant& b);
Variant opMul(const Variant& a, const Variant& b);
Variant opDiv(const Variant& a, const Variant& b);     // / : float division
Variant opIDiv(const Variant& a, const Variant& b);    // \ : integer division
Variant opMod(const Variant& a, const Variant& b);
Variant opPow(const Variant& a, const Variant& b);     // ^
Variant opNeg(const Variant& a);
Variant opNot(const Variant& a);                       // bitwise for ints, logical otherwise
Variant opLogic(const std::string& op, const Variant& a, const Variant& b); // and or xor eqv imp
Variant opCompare(const std::string& op, const Variant& a, const Variant& b, bool compareText);
bool variantIsTrue(const Variant& v);                  // == toBool but Null -> Null handled by caller
bool objSame(const Variant& a, const Variant& b);      // for Is
