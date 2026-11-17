' SPDX-License-Identifier: BSD-2-Clause
' Copyright (c) 2026 Lucas Holt. All rights reserved.

' error handling
On Error Resume Next
Dim x
x = 1 / 0
WScript.Echo "after div0: err=" & Err.Number & " " & Err.Description
Err.Clear
WScript.Echo "cleared: " & Err.Number

x = "abc" + 1
WScript.Echo "type mismatch: " & Err.Number
Err.Clear

On Error Goto 0
WScript.Echo "handler off"

' Err.Raise
On Error Resume Next
Err.Raise 1234, "mysource", "custom problem"
WScript.Echo Err.Number, Err.Source, Err.Description
On Error Goto 0

' error inside a called procedure propagates to caller's handler
Sub Boom()
    Dim y
    y = 1 / 0
End Sub
On Error Resume Next
Boom
WScript.Echo "boom err=" & Err.Number
On Error Goto 0

' Null propagation
Dim n
n = Null
WScript.Echo IsNull(n + 1), IsNull(n & "x"), IsNull(n = 5)
On Error Resume Next
Dim z
z = n + 1
WScript.Echo "null arith err=" & Err.Number
On Error Goto 0

' subscript out of range
On Error Resume Next
Dim a(2)
a(5) = 1
WScript.Echo "subscript err=" & Err.Number
On Error Goto 0
WScript.Echo "survived"
