' SPDX-License-Identifier: BSD-2-Clause
' Copyright (c) 2026 Lucas Holt. All rights reserved.

' gaps vs the documented MS VBScript surface: Like operator, Filter,
' FormatCurrency, GetLocale/SetLocale, vbUseSystemDayOfWeek

' ---- Like operator (pattern: ? * # [list] [!list]) ----
WScript.Echo "abc" Like "a*"          ' True
WScript.Echo "abc" Like "???"         ' True
WScript.Echo "abc" Like "abd"         ' False
WScript.Echo "abc" Like "*"           ' True
WScript.Echo "" Like "*"              ' True
WScript.Echo "a5" Like "a#"           ' True
WScript.Echo "ab" Like "a#"           ' False
WScript.Echo "abc" Like "[a-c]bc"     ' True
WScript.Echo "xbc" Like "[a-c]bc"     ' False
WScript.Echo "xbc" Like "[!a-c]bc"    ' True
WScript.Echo "b" Like "[a-c]"         ' True
WScript.Echo "a-b" Like "a[ -~]b"     ' True (range)
WScript.Echo "abc" Like "ABC"         ' False (binary compare)
WScript.Echo "a]c" Like "a[]]c"       ' True (] literal first in list)
WScript.Echo "a[c" Like "a[[]c"       ' True ([ literal via [[])
WScript.Echo "hello" Like "h*o"       ' True
WScript.Echo "hello" Like "h*l*o*"    ' True

' ---- Filter ----
Dim a, f
a = Array("apple", "banana", "grape")
f = Filter(a, "ap")
WScript.Echo Join(f, ",")             ' apple,grape ("ap" in both)
f = Filter(a, "an")
WScript.Echo Join(f, ",")             ' banana
f = Filter(a, "e", False)
WScript.Echo Join(f, ",")             ' banana (only one without "e")
f = Filter(a, "A", True, vbTextCompare)
WScript.Echo Join(f, ",")             ' apple,banana,grape (all contain "a")
On Error Resume Next
f = Filter(a, "zzz")
WScript.Echo IsArray(f)               ' True
WScript.Echo UBound(f)                ' error 9 (uninitialized array), like MS
WScript.Echo "err=" & Err.Number
On Error Goto 0

' ---- FormatCurrency ----
WScript.Echo FormatCurrency(1234.5)              ' $1,234.50
WScript.Echo FormatCurrency(1234.567, 2)         ' $1,234.57
WScript.Echo FormatCurrency(-1234.5)             ' ($1,234.50)
WScript.Echo FormatCurrency(-1234.5, 2, vbTrue, vbFalse) ' -$1,234.50
WScript.Echo FormatCurrency(0.5)                 ' $0.50
WScript.Echo FormatCurrency(0.5, 2, vbFalse)     ' $.50
WScript.Echo FormatCurrency(1234.6, 0, vbTrue, vbTrue, vbFalse) ' $1235

' ---- GetLocale / SetLocale ----
WScript.Echo GetLocale()              ' 1033
WScript.Echo SetLocale(2057)          ' 1033 (previous)
WScript.Echo GetLocale()              ' 2057
WScript.Echo SetLocale("en-gb")       ' 2057
WScript.Echo GetLocale()              ' en-gb
WScript.Echo SetLocale(1033)          ' en-gb
WScript.Echo GetLocale()              ' 1033

' ---- vbUseSystemDayOfWeek ----
WScript.Echo vbUseSystemDayOfWeek     ' 1
WScript.Echo Weekday(#2023-01-01#, vbUseSystemDayOfWeek) ' 1 (Sunday)
WScript.Echo Weekday(#2023-01-01#, vbMonday)             ' 7
