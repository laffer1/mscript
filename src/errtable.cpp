// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 Lucas Holt. All rights reserved.

// VBScript error numbers, their descriptions, and the translation of COM
// HRESULTs into VBScript error numbers.
//
// Provenance: the tables below were written from Microsoft's published
// documentation, listed here; each entry cites its source.
//
//   [VBS]  VBScript Run-time Errors (VBScript language reference)
//          https://learn.microsoft.com/previous-versions//xe43cc8d(v=vs.85)
//   [VBA]  Trappable errors (Office VBA language reference)
//          https://learn.microsoft.com/office/vba/language/reference/user-interface-help/trappable-errors
//   [COM1] COM Error Codes (Generic)
//          https://learn.microsoft.com/windows/win32/com/com-error-codes-1
//   [COM2] COM Error Codes (XACT, SCHED, OLE)
//          https://learn.microsoft.com/windows/win32/com/com-error-codes-2
//   [COM3] COM Error Codes (STG, RPC)
//          https://learn.microsoft.com/windows/win32/com/com-error-codes-3
//   [HRS]  Structure of COM Error Codes
//          https://learn.microsoft.com/windows/win32/com/structure-of-com-error-codes
//   [SYS]  System Error Codes (1700-3999)
//          https://learn.microsoft.com/windows/win32/debug/system-error-codes--1700-3999-
//
// A HRESULT is translated only when the documented meaning of the code
// corresponds to exactly one documented VBScript/VB error; anything else is
// passed through unchanged, as before.

#include "variant.h"

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <string>
#include <string_view>

namespace {

// ---------- error number -> description ----------

struct ErrText {
    int num;
    std::string_view text;
};

// Sorted by error number (checked at compile time below).
constexpr ErrText kErrText[] = {
    {5,    "Invalid procedure call or argument"},            // [VBS]
    {6,    "Overflow"},                                      // [VBS]
    {7,    "Out of memory"},                                 // [VBS] "Out of Memory"
    {9,    "Subscript out of range"},                        // [VBS]
    {10,   "This array is fixed or temporarily locked"},     // [VBS]
    {11,   "Division by zero"},                              // [VBS]
    {13,   "Type mismatch"},                                 // [VBS]
    {14,   "Out of string space"},                           // [VBS]
    {17,   "Can't perform requested operation"},             // [VBS]
    {28,   "Out of stack space"},                            // [VBS]
    {35,   "Sub or Function not defined"},                   // [VBS] "Sub or function not defined"
    {48,   "Error in loading DLL"},                          // [VBS]
    {51,   "Internal error"},                                // [VBS]
    {52,   "Bad file name or number"},                       // [VBA]
    {53,   "File not found"},                                // [VBA]
    {57,   "Device I/O error"},                              // [VBA]
    {58,   "File already exists"},                           // [VBA]
    {61,   "Disk full"},                                     // [VBA]
    {62,   "Input past end of file"},                        // [VBA]
    {67,   "Too many files"},                                // [VBA]
    {70,   "Permission denied"},                             // [VBA]
    {75,   "Path/File access error"},                        // [VBA]
    {76,   "Path not found"},                                // [VBA]
    {91,   "Object variable not set"},                       // [VBS]
    {92,   "For loop not initialized"},                      // [VBS]
    {94,   "Invalid use of Null"},                           // [VBS]
    {322,  "Can't create necessary temporary file"},         // [VBA]
    {424,  "Object required"},                               // [VBS]
    {425,  "Invalid object"},                                // mscript (kept)
    {429,  "ActiveX component can't create object"},         // [VBS]
    {430,  "Class doesn't support Automation"},              // [VBS]
    {432,  "File name or class name not found during Automation operation"}, // [VBS]
    {438,  "Object doesn't support this property or method"}, // [VBS]
    {445,  "Object doesn't support this action"},            // [VBS]
    {446,  "Object doesn't support named arguments"},        // [VBA]
    {447,  "Object doesn't support current locale setting"}, // [VBS]
    {448,  "Named argument not found"},                      // [VBS]
    {449,  "Argument not optional"},                         // [VBS]
    {450,  "Wrong number of arguments or invalid property assignment"}, // [VBS]
    {451,  "Object not a collection"},                       // [VBS]
    {458,  "Variable uses an Automation type not supported in VBScript"}, // [VBS]
    {462,  "The remote server machine does not exist or is unavailable"}, // [VBS]
    {481,  "Invalid picture"},                               // [VBS]
    {500,  "Variable is undefined"},                         // [VBS]
    {501,  "Invalid assignment"},                            // mscript (kept)
    {502,  "Object not safe for scripting"},                 // [VBS]
    {503,  "Object not safe for initializing"},              // [VBS]
    {504,  "Object not safe for creating"},                  // [VBS]
    {505,  "Invalid or unqualified reference"},              // [VBS]
    {506,  "Class not defined"},                             // [VBS]
    {507,  "An exception occurred"},                         // [VBS]
    {1001, "Out of memory"},                                 // mscript (kept)
    {5008, "Illegal assignment"},                            // [VBS]
    {5017, "Syntax error in regular expression"},            // [VBS]
    {5018, "Unexpected quantifier"},                         // [VBS]
    {5019, "Expected ']' in regular expression"},            // [VBS]
    {5020, "Expected ')' in regular expression"},            // [VBS]
    {5021, "Invalid range in character set"},                // [VBS]
};

// ---------- HRESULT -> error number ----------

// [HRS]: bit 31 is the severity (1 = failure), bits 16..26 the facility,
// bits 0..15 the code.
constexpr uint32_t kSeverityError   = 0x80000000u;
constexpr unsigned kFacilityNull     = 0x0;
constexpr unsigned kFacilityDispatch = 0x2;
constexpr unsigned kFacilityStorage  = 0x3;
constexpr unsigned kFacilityItf      = 0x4;
constexpr unsigned kFacilityWin32    = 0x7;
constexpr unsigned kFacilityVBS      = 0xA; // VBScript's own errors: code = VB number

struct CodeMap {
    uint16_t code; // low 16 bits of the HRESULT
    uint16_t vb;   // VBScript error number
};

// Each table is sorted by code. Comments give the constant and Microsoft's
// documented description of it.

constexpr CodeMap kNull[] = {     // [COM1]
    {0x4001, 445}, // E_NOTIMPL      "Not implemented"
    {0x4002, 430}, // E_NOINTERFACE  "No such interface supported"
};

constexpr CodeMap kDispatch[] = { // [COM2]
    {0x0003, 438}, // DISP_E_MEMBERNOTFOUND   "Member not found"
    {0x0004, 448}, // DISP_E_PARAMNOTFOUND    "Parameter not found"
    {0x0005, 13},  // DISP_E_TYPEMISMATCH     "Type mismatch"
    {0x0006, 438}, // DISP_E_UNKNOWNNAME      "Unknown name"
    {0x0007, 446}, // DISP_E_NONAMEDARGS      "No named arguments"
    {0x0008, 458}, // DISP_E_BADVARTYPE       "Bad variable type"
    {0x000A, 6},   // DISP_E_OVERFLOW         "Out of present range"
    {0x000B, 9},   // DISP_E_BADINDEX         "Invalid index"
    {0x000C, 447}, // DISP_E_UNKNOWNLCID      "Unknown language"
    {0x000D, 10},  // DISP_E_ARRAYISLOCKED    "Memory is locked"
    {0x000E, 450}, // DISP_E_BADPARAMCOUNT    "Invalid number of parameters"
    {0x000F, 449}, // DISP_E_PARAMNOTOPTIONAL "Parameter not optional"
    {0x0011, 451}, // DISP_E_NOTACOLLECTION   "Does not support a collection"
    {0x0012, 11},  // DISP_E_DIVBYZERO        "Division by zero"
    {0x802A, 13},  // TYPE_E_WRONGTYPEKIND    "Type mismatch"
    {0x802F, 453}, // TYPE_E_DLLFUNCTIONNOTFOUND "Function not defined in specified DLL"
    {0x8CA0, 13},  // TYPE_E_TYPEMISMATCH     "Type mismatch"
    {0x8CA2, 57},  // TYPE_E_IOERROR          "I/O Error"
    {0x8CA3, 322}, // TYPE_E_CANTCREATETMPFILE "Error creating unique tmp file"
    {0x9C4A, 48},  // TYPE_E_CANTLOADLIBRARY  "Error loading type library/DLL"
};

constexpr CodeMap kStorage[] = {  // [COM3]; VB numbers per [VBA]
    {0x0002, 53},  // STG_E_FILENOTFOUND       "... could not be found"
    {0x0003, 76},  // STG_E_PATHNOTFOUND       "The path %1 could not be found"
    {0x0004, 67},  // STG_E_TOOMANYOPENFILES   "insufficient resources to open another file"
    {0x0005, 70},  // STG_E_ACCESSDENIED       "Access Denied"
    {0x0008, 7},   // STG_E_INSUFFICIENTMEMORY "insufficient memory available"
    {0x0013, 70},  // STG_E_DISKISWRITEPROTECTED "Disk is write-protected"
    {0x001D, 57},  // STG_E_WRITEFAULT         "A disk error occurred during a write operation"
    {0x001E, 57},  // STG_E_READFAULT          "A disk error occurred during a read operation"
    {0x0020, 70},  // STG_E_SHAREVIOLATION     "A share violation has occurred"
    {0x0021, 70},  // STG_E_LOCKVIOLATION      "A lock violation has occurred"
    {0x0050, 58},  // STG_E_FILEALREADYEXISTS  "... already exists"
    {0x0070, 61},  // STG_E_MEDIUMFULL         "insufficient disk space"
    {0x00FC, 52},  // STG_E_INVALIDNAME        "The name %1 is not valid"
};

constexpr CodeMap kItf[] = {      // [COM1]
    {0x0111, 429}, // CLASS_E_CLASSNOTAVAILABLE "ClassFactory cannot supply requested class"
    {0x0154, 429}, // REGDB_E_CLASSNOTREG       "Class not registered"
    {0x01F3, 429}, // CO_E_CLASSSTRING          "Invalid class string"
    {0x01F5, 429}, // CO_E_APPNOTFOUND          "Application not found"
};

constexpr CodeMap kWin32[] = {    // [COM1], [SYS]: low word is the Win32 error
    {5,    70},    // E_ACCESSDENIED   "General access denied error"
    {14,   7},     // E_OUTOFMEMORY    "Ran out of memory"
    {87,   5},     // E_INVALIDARG     "One or more arguments are invalid"
    {1722, 462},   // RPC_S_SERVER_UNAVAILABLE "The RPC server is unavailable"
};

template <typename T, size_t N, typename Key>
constexpr bool sortedBy(const T (&a)[N], Key key) {
    for (size_t i = 1; i < N; i++)
        if (!(key(a[i - 1]) < key(a[i]))) return false;
    return true;
}
constexpr auto byNum = [](const ErrText& e) { return e.num; };
constexpr auto byCode = [](const CodeMap& m) { return m.code; };
static_assert(sortedBy(kErrText, byNum), "kErrText must be sorted by number");
static_assert(sortedBy(kNull, byCode) && sortedBy(kDispatch, byCode) &&
              sortedBy(kStorage, byCode) && sortedBy(kItf, byCode) &&
              sortedBy(kWin32, byCode), "HRESULT tables must be sorted by code");

template <size_t N>
int lookup(const CodeMap (&table)[N], uint16_t code) {
    auto it = std::lower_bound(std::begin(table), std::end(table), code,
                               [](const CodeMap& m, uint16_t c) { return m.code < c; });
    return (it != std::end(table) && it->code == code) ? it->vb : -1;
}

} // namespace

std::string errDesc(int num) {
    auto it = std::lower_bound(std::begin(kErrText), std::end(kErrText), num,
                               [](const ErrText& e, int n) { return e.num < n; });
    if (it != std::end(kErrText) && it->num == num) return std::string(it->text);
    return "Runtime error " + std::to_string(num);
}

[[noreturn]] void throwErr(int num, const std::string& desc) {
    throw VBSError(num, desc.empty() ? errDesc(num) : desc);
}

// Translate a raw HRESULT into the value VBScript reports as Err.Number.
int mapHRESULTToVB(uint32_t hres) {
    if (!(hres & kSeverityError)) return (int)hres;
    const unsigned facility = (hres >> 16) & 0x7FFu;
    const uint16_t code = (uint16_t)(hres & 0xFFFFu);
    int vb = -1;
    switch (facility) {
        case kFacilityVBS:      return code;
        case kFacilityNull:     vb = lookup(kNull, code); break;
        case kFacilityDispatch: vb = lookup(kDispatch, code); break;
        case kFacilityStorage:  vb = lookup(kStorage, code); break;
        case kFacilityItf:      vb = lookup(kItf, code); break;
        case kFacilityWin32:    vb = lookup(kWin32, code); break;
        default: break;
    }
    return vb >= 0 ? vb : (int)hres;
}
