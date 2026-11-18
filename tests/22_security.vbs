' SPDX-License-Identifier: BSD-2-Clause
' Copyright (c) 2026 Lucas Holt. All rights reserved.
Option Explicit

' Security regression tests: resource limits must raise clean VBScript
' errors, not crash the process.
Dim a, i, s, d, cn, rs, q

On Error Resume Next

' --- array dimension product overflow -> error 7 (was heap OOB write) ---
Dim big(4294967296, 4294967296)
WScript.Echo "arr-ovf=" & Err.Number
Err.Clear
' negative dim -> error 5
Dim neg(-1)
WScript.Echo "arr-neg=" & Err.Number
Err.Clear
' legal array still works
Dim ok(10, 20)
ok(3, 4) = 7
WScript.Echo "arr-ok=" & ok(3, 4)

' --- huge string alloc -> error 14 (was uncaught bad_alloc) ---
s = Space(2000000000)
WScript.Echo "space-cap=" & Err.Number
Err.Clear
s = String(2000000000, "x")
WScript.Echo "string-cap=" & Err.Number
Err.Clear
WScript.Echo "space-ok=[" & Space(5) & "]"

' --- unbounded procedure recursion -> error 28 (was stack overflow) ---
Sub Recurse
    Recurse
End Sub
Recurse
WScript.Echo "recursion=" & Err.Number
Err.Clear

On Error Goto 0

' --- XML nesting depth cap ---
Set d = CreateObject("MSXML2.DOMDocument")
s = ""
For i = 1 To 600
    s = s & "<a>"
Next
For i = 1 To 600
    s = s & "</a>"
Next
WScript.Echo "xml-deep=" & d.loadXML(s) & "|" & d.parseError.reason
WScript.Echo "xml-ok=" & d.loadXML("<r><a><b>1</b></a></r>") & "|" & d.documentElement.nodeName

' --- ADODB: quoted identifier with embedded quote round-trips through update ---
q = Chr(34)
Set cn = CreateObject("ADODB.Connection")
cn.Open ":memory:"
cn.Execute "CREATE TABLE " & q & "we" & q & q & "ird" & q & " (id INTEGER PRIMARY KEY, name TEXT)"
cn.Execute "INSERT INTO " & q & "we" & q & q & "ird" & q & " (name) VALUES ('q')"
Set rs = cn.Execute("SELECT * FROM " & q & "we" & q & q & "ird" & q)
rs("name") = "q2"
rs.Update
Set rs = cn.Execute("SELECT name FROM " & q & "we" & q & q & "ird" & q & " WHERE id=1")
WScript.Echo "adodb-quote=" & rs("name")
