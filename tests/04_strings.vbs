' SPDX-License-Identifier: BSD-2-Clause
' Copyright (c) 2026 Lucas Holt. All rights reserved.

' string functions
Dim s
s = "  Hello, World!  "
WScript.Echo "[" & Trim(s) & "]"
WScript.Echo "[" & LTrim(s) & "]"
WScript.Echo "[" & RTrim(s) & "]"
WScript.Echo UCase("abc"), LCase("AbC")
WScript.Echo Left("abcdef", 3), Right("abcdef", 3), Mid("abcdef", 2, 3), Mid("abcdef", 4)
WScript.Echo Len("hello"), Len("")
WScript.Echo Space(3) & "|"
WScript.Echo String(4, "ab"), String(3, 65)
WScript.Echo StrReverse("abcde")
WScript.Echo StrComp("abc", "abd"), StrComp("abc", "abc"), StrComp("abd", "abc")
WScript.Echo StrComp("abc", "ABC", vbTextCompare), StrComp("abc", "ABC", vbBinaryCompare)
WScript.Echo InStr("hello world", "world"), InStr("hello", "z"), InStr(2, "hello", "l")
WScript.Echo InStrRev("hello world", "o"), InStrRev("a.b.c", ".")
WScript.Echo Replace("aaa bbb aaa", "aaa", "ccc")
WScript.Echo Replace("aAa", "A", "b", 1, 1, vbTextCompare)
Dim parts
parts = Split("a,b,c", ",")
WScript.Echo UBound(parts), parts(0), parts(1), parts(2)
WScript.Echo Join(parts, "-")
WScript.Echo Join(Array("x", "y"), "")
WScript.Echo Asc("A"), Chr(65), Asc("Z"), Chr(122)
WScript.Echo Hex(255), Hex(-1), Oct(8), Oct(64)
WScript.Echo "tab:" & vbTab & "end"
WScript.Echo "crlf lines:" & vbCrLf & "second"
WScript.Echo InStr(1, "Mississippi", "ss", vbTextCompare)
WScript.Echo InStr("Mississippi", "SSI", vbTextCompare)
