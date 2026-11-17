' SPDX-License-Identifier: BSD-2-Clause
' Copyright (c) 2026 Lucas Holt. All rights reserved.

' control flow
Dim i, n
n = 0

If n = 0 Then
    WScript.Echo "zero"
ElseIf n = 1 Then
    WScript.Echo "one"
Else
    WScript.Echo "other"
End If

If n = 0 Then WScript.Echo "single-line if"
If n = 1 Then WScript.Echo "no" Else WScript.Echo "single-line else"

Select Case n
    Case 0
        WScript.Echo "case 0"
    Case 1, 2
        WScript.Echo "case 1 or 2"
    Case Else
        WScript.Echo "case else"
End Select

Select Case "apple"
    Case "banana"
        WScript.Echo "banana"
    Case "apple"
        WScript.Echo "apple"
    Case Else
        WScript.Echo "else"
End Select

Select Case 10
    Case Is < 5
        WScript.Echo "lt 5"
    Case Is >= 5
        WScript.Echo "ge 5"
End Select

For i = 1 To 5
    WScript.Echo "for " & i
Next

For i = 10 To 1 Step -3
    WScript.Echo "down " & i
Next

Dim total
total = 0
For i = 1 To 10
    If i > 5 Then Exit For
    total = total + i
Next
WScript.Echo "total=" & total

Dim arr
arr = Array(10, 20, 30)
For Each i In arr
    WScript.Echo "each " & i
Next

i = 0
Do While i < 3
    WScript.Echo "dowhile " & i
    i = i + 1
Loop

i = 0
Do Until i >= 3
    i = i + 1
Loop
WScript.Echo "dountil " & i

i = 0
Do
    i = i + 1
Loop While i < 3
WScript.Echo "loopwhile " & i

i = 0
Do
    i = i + 1
    If i = 2 Then Exit Do
Loop
WScript.Echo "exitdo " & i

i = 0
While i < 3
    WScript.Echo "while " & i
    i = i + 1
Wend

' nested loops with exit
For i = 1 To 3
    For n = 1 To 3
        If n = 2 Then Exit For
        WScript.Echo "nested " & i & "," & n
    Next
Next
