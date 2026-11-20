' SPDX-License-Identifier: BSD-2-Clause
' Copyright (c) 2026 Lucas Holt. All rights reserved.

' vbObjectError is &H80040000 (a Long), and Err.Raise vbObjectError + n
' keeps that number in Err.Number
WScript.Echo vbObjectError, Hex(vbObjectError), VarType(vbObjectError)
WScript.Echo vbObjectError = &H80040000

On Error Resume Next
Err.Raise vbObjectError + 1, "MyLib", "custom one"
WScript.Echo Err.Number, Hex(Err.Number), Err.Number - vbObjectError
WScript.Echo Err.Number = vbObjectError + 1, Err.Source, Err.Description
Err.Clear

Const ERR_NOT_READY = 513
Err.Raise vbObjectError + ERR_NOT_READY, "MyLib", "not ready"
WScript.Echo Hex(Err.Number), Err.Number - vbObjectError, Err.Description
Err.Clear

' a class raising its own error, caught by the caller
Class Widget
    Public Sub Fail()
        Err.Raise vbObjectError + 1000, "Widget", "widget failed"
    End Sub
End Class
Dim w
Set w = New Widget
w.Fail
WScript.Echo Err.Number - vbObjectError, Err.Source, Err.Description
On Error Goto 0
