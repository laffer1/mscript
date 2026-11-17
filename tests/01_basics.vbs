' SPDX-License-Identifier: BSD-2-Clause
' Copyright (c) 2026 Lucas Holt. All rights reserved.

' basics: output, arithmetic, strings, coercion, comments, colons, continuation
WScript.Echo "Hello, World!"

Dim a, b, c
a = 10
b = 3
WScript.Echo a + b, a - b, a * b, a / b, a \ b, a Mod b, a ^ b

' string concatenation and mixing
Dim s
s = "foo" & "bar"
WScript.Echo s, Len(s), "n=" & 42

' + with two strings concatenates; + with numbers adds
WScript.Echo "1" + "2", "1" + 2, 1 + "2"

' coercion
WScript.Echo CStr(123), CInt("42") + 1, CBool(0), CBool(-1), CBool("True")
WScript.Echo Val("  3.5abc"), IsNumeric("12"), IsNumeric("12a"), IsNumeric("&H1F")

' comments and multiple statements per line
x = 1 : y = 2 : WScript.Echo x + y

' line continuation
WScript.Echo "a long " & _
             "continued string"

' types
WScript.Echo TypeName(1), TypeName(40000), TypeName(1.5), TypeName("s"), TypeName(True), TypeName(Now)
WScript.Echo VarType(1), VarType(40000), VarType(1.5), VarType("s"), VarType(True)

' True is -1, False is 0
WScript.Echo True, False, CInt(True), CInt(False)

' hex and octal literals
WScript.Echo &HFF, &o17, &H10 + 1

' Empty behaves as 0 / ""
Dim e
WScript.Echo e + 5, "x" & e, IsEmpty(e), VarType(e)

' Rem comment
Rem this is a rem comment
WScript.Echo "done"
