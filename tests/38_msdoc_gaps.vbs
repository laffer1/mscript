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
WScript.Echo "aaaa" Like "*a*a*a*b"   ' False (backtracking, no stall)
WScript.Echo IsNull(Null Like "a*")   ' True (Null propagates)
On Error Resume Next
WScript.Echo "a" Like "[z-a]"         ' error 93: descending range
WScript.Echo "err=" & Err.Number
Err.Clear
WScript.Echo "a [xyz" Like "a [*&"    ' error 93: unterminated [
WScript.Echo "err=" & Err.Number
On Error Goto 0

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
f = Filter(Array("Ärger", "Zebra"), "är", True, vbTextCompare)
WScript.Echo Join(f, ",")             ' Ärger (unicode case fold)
On Error Resume Next
f = Filter(a, "zzz")
WScript.Echo IsArray(f)               ' True
WScript.Echo UBound(f)                ' error 9 (uninitialized array), like MS
WScript.Echo "err=" & Err.Number
On Error Goto 0
ReDim Preserve f(2)                   ' ReDim works on the empty result
f(0) = "x"
WScript.Echo Join(f, ",")             ' x,,

' ---- FormatCurrency ----
WScript.Echo FormatCurrency(1234.5)              ' $1,234.50
WScript.Echo FormatCurrency(1234.567, 2)         ' $1,234.57
WScript.Echo FormatCurrency(-1234.5)             ' ($1,234.50)
WScript.Echo FormatCurrency(-1234.5, 2, vbTrue, vbFalse) ' -$1,234.50
WScript.Echo FormatCurrency(0.5)                 ' $0.50
WScript.Echo FormatCurrency(0.5, 2, vbFalse)     ' $.50
WScript.Echo FormatCurrency(1234.6, 0, vbTrue, vbTrue, vbFalse) ' $1235
WScript.Echo FormatCurrency(1.23456789012345e15, 2) ' $1,234,567,890,123,450.00 (no truncation)
SetLocale("de-de")
WScript.Echo FormatCurrency(1234.5)              ' 1.234,50 € (trailing symbol in de-DE)
SetLocale(1033)
On Error Resume Next
WScript.Echo FormatCurrency(1, 100)              ' error 5: bad digit count
WScript.Echo "err=" & Err.Number
Err.Clear
WScript.Echo FormatCurrency(1, 2, 5)             ' error 5: bad tri-state
WScript.Echo "err=" & Err.Number
On Error Goto 0

' ---- GetLocale / SetLocale ----
WScript.Echo GetLocale()              ' 1033
WScript.Echo SetLocale(2057)          ' 1033 (previous)
WScript.Echo GetLocale()              ' 2057
WScript.Echo SetLocale("en-gb")       ' 2057 (names resolve to the LCID)
WScript.Echo GetLocale()              ' 2057
WScript.Echo SetLocale(1033)          ' 2057
WScript.Echo GetLocale()              ' 1033
On Error Resume Next
WScript.Echo SetLocale("xx-yy")       ' error 5: unknown locale name
WScript.Echo "err=" & Err.Number
On Error Goto 0

' ---- vbUseSystemDayOfWeek (system setting, here: locale first day) ----
WScript.Echo vbUseSystemDayOfWeek     ' 1
SetLocale(1033)                       ' en-US: first day is Sunday
WScript.Echo Weekday(#2023-01-01#, vbUseSystemDayOfWeek) ' 1 (Sunday)
SetLocale("en-gb")                    ' en-GB: first day is Monday
WScript.Echo Weekday(#2023-01-01#, vbUseSystemDayOfWeek) ' 7 (Sunday is last)
SetLocale(1033)
WScript.Echo Weekday(#2023-01-01#, vbMonday)             ' 7 (explicit)
