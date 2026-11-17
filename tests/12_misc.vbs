' SPDX-License-Identifier: BSD-2-Clause
' Copyright (c) 2026 Lucas Holt. All rights reserved.

' misc: logical ops, comparisons, conversions, misc functions
WScript.Echo IIf(3 > 2, "yes", "no"), IIf(1 > 2, "yes", "no")
WScript.Echo Choose(2, "a", "b", "c"), IsNull(Choose(9, "a"))
WScript.Echo Switch(True, "t1", False, "f1", True, "t2")

' bitwise on integers, logical on booleans
WScript.Echo 5 And 3, 5 Or 3, 5 Xor 3, Not 5
WScript.Echo True And False, True Or False, Not True, True Xor True
WScript.Echo True Eqv True, True Imp False, False Imp True

' string comparison is binary (case-sensitive) by default
WScript.Echo "abc" = "abc", "abc" = "ABC", "abc" <> "ABC"
Option Compare Text
WScript.Echo "abc" = "ABC"
Option Compare Binary

' Null propagates through comparisons and logic
Dim n
n = Null
WScript.Echo IsNull(n < 5), IsNull(Not n), IsNull(n And True)

WScript.Echo Abs(-3.5), Abs(-3), Sgn(-2.5), Sgn(0), Sgn(9)
WScript.Echo Int(-3.5), Fix(-3.5), Int(3.5), Fix(3.5)
WScript.Echo Round(2.5), Round(3.5), Round(-2.5), Round(2.567, 2)
WScript.Echo Sqr(16), Sqr(2)
WScript.Echo RGB(255, 0, 0), Hex(RGB(255, 0, 0))
WScript.Echo IsEmpty(Empty), IsNull(Null), IsArray(Array()), IsObject(CreateObject("Scripting.Dictionary"))
WScript.Echo TypeName(Nothing), IsObject(Nothing)

' operator precedence: & is lower than +
WScript.Echo "n=" & 1 + 2
WScript.Echo 2 + 3 & "x"

' Imp/Eqv precedence with And/Or
WScript.Echo (True And False) Or True

' string escapes: "" inside string
WScript.Echo "say ""hi"""

' bracketed identifiers
Dim [my var]
[my var] = 42
WScript.Echo [my var]

' ScriptEngine
WScript.Echo ScriptEngine, ScriptEngineMajorVersion & "." & ScriptEngineMinorVersion
