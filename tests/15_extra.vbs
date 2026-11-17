' SPDX-License-Identifier: BSD-2-Clause
' Copyright (c) 2026 Lucas Holt. All rights reserved.

' extra features: Static, Case ranges, date arithmetic, Format, single-line procs
Option Explicit

Sub Bump()
    Static n
    n = n + 1
    WScript.Echo "bump " & n
End Sub
Bump
Bump
Bump

Dim x
x = 7
Select Case x
    Case 1 To 5
        WScript.Echo "low"
    Case 6, 7, 8
        WScript.Echo "mid"
    Case Is > 100
        WScript.Echo "high"
    Case Else
        WScript.Echo "else"
End Select

Dim d
d = CDate("3/15/2024")
WScript.Echo d + 1, d - 1, d - CDate("3/10/2024"), TypeName(d + 1), TypeName(d - d)
WScript.Echo d > CDate("3/1/2024"), d = CDate("3/15/2024")

WScript.Echo Format(1234.5678, "#,##0.00"), Format(0.5, "0%"), FormatNumber(3.14159, 3)
WScript.Echo FormatDateTime(CDate("2024-03-15"), 1)
WScript.Echo FormatDateTime(CDate("2024-03-15") + TimeSerial(9, 5, 0), 4)

' single-line sub with colon-separated body
Sub Hello(name) : WScript.Echo "hello " & name : End Sub
Hello "world"

' Exit Sub early
Sub Early(n)
    If n > 0 Then
        WScript.Echo "positive"
        Exit Sub
    End If
    WScript.Echo "non-positive"
End Sub
Early 1
Early -1

' nested With
Class Inner
    Public v
End Class
Class Outer
    Public inner
    Public name
    Private Sub Class_Initialize()
        Set inner = New Inner
        inner.v = 5
        name = "outer"
    End Sub
End Class
Dim o
Set o = New Outer
With o
    WScript.Echo .name
    With .inner
        WScript.Echo .v
    End With
End With

' On Error Goto label inside a procedure
Sub Guarded()
    On Error GoTo Oops
    Dim z
    z = 10 / 0
    WScript.Echo "not reached"
    Exit Sub
Oops:
    WScript.Echo "oops " & Err.Number
    Err.Clear
End Sub
Guarded

' string building in a loop
Dim sb, i
sb = ""
For i = 1 To 5
    sb = sb & i
    If i < 5 Then sb = sb & "-"
Next
WScript.Echo sb

' ternary-ish via IIf and nested calls
WScript.Echo IIf(Len("abc") > 2, UCase("big"), "small")
