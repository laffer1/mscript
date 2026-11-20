' SPDX-License-Identifier: BSD-2-Clause
' Copyright (c) 2026 Lucas Holt. All rights reserved.

' & treats a single Null operand as ""; only Null & Null is Null.
' + keeps its own rule: any Null operand gives Null.
Dim n
n = Null
WScript.Echo "[" & ("text" & Null) & "]", "[" & (Null & "text") & "]"
WScript.Echo IsNull("text" & n), VarType("text" & n), VarType(n & 1)
WScript.Echo "[" & (n & 42) & "]", "[" & (3.5 & n) & "]", "[" & (True & n) & "]"
WScript.Echo IsNull(n & n), IsNull(Null & Null), VarType(n & n)
WScript.Echo "[" & (n & n & "x") & "]"
WScript.Echo IsNull(Empty & n), VarType(Empty & n), Len(Empty & n)
WScript.Echo "name: " & n & "!"
Dim s
s = "a"
s = s & n
WScript.Echo "[" & s & "]"
' + still propagates Null, for strings and numbers
WScript.Echo IsNull("text" + n), IsNull(n + "text"), IsNull(1 + n), IsNull(n + n)
