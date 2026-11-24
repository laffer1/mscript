' SPDX-License-Identifier: BSD-2-Clause
' Copyright (c) 2026 Lucas Holt. All rights reserved.

' JSON extension (not part of VBScript): JSON.Parse / JSON.Stringify
Dim o
Set o = JSON.Parse("{""name"":""mscript"",""ver"":1,""tags"":[""a"",""b""],""ok"":true,""nothing"":null,""pi"":3.5}")
WScript.Echo o.name & " " & o.ver & " " & o.tags(1) & " " & o.ok & " " & IsNull(o.nothing) & " " & o.pi
WScript.Echo JSON.Stringify(o)
WScript.Echo JSON.Stringify(Array(1, "x", true, Null))
WScript.Echo JSON.Stringify(#2026-11-24 13:14:15#)
WScript.Echo JSON.Stringify(o, Null, 2)

Dim d
Set d = JSON.Parse("{""a"":1}")
d.b = Array(1, 2)
WScript.Echo JSON.Stringify(d)
WScript.Echo JSON.Stringify(d, Array("a"))

On Error Resume Next
JSON.Parse("{bad")
WScript.Echo "err=" & Err.Number
On Error Goto 0
